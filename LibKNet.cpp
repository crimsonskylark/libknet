#include <ntifs.h>
#include <stdarg.h>

#include "LibKNet.hpp"

#define REFLECT( Name ) ( UINT8* )( ( VOID* )( #Name ) )

static inline UINT64 Timestamp( ) {
  LARGE_INTEGER SystemTime{ };
  KeQuerySystemTimePrecise( &SystemTime );
  return SystemTime.QuadPart;
}

static inline ULONG64 UnsafeRangeRng( ULONG64 Min, ULONG64 Max ) {
  auto const Seed = __rdtsc( );
  ULONG64    Mix = Seed ^ ( Seed >> 32u );

  Mix *= 0xbea225f9eb34556d;
  Mix ^= Mix >> 29u;

  return Min + ( Mix % ( Max - Min + 1u ) );
}

_IRQL_requires_max_(
    DISPATCH_LEVEL
) NTSTATUS GenericIrpCompletion( PDEVICE_OBJECT DevObj, PIRP Irp, PVOID Context ) {
  UNREFERENCED_PARAMETER( DevObj );
  NTSTATUS           Status = Irp->IoStatus.Status;
  NTSTATUS           SelfStatus{ };
  PASYNC_MSG         Self = ( PASYNC_MSG )( Context );
  PIRP_ASYNC_CONTEXT Ias = &Self->Ias;
  BOOLEAN            IsRX = Self->IsRX( );
  BOOLEAN            IsTX = IsRX == FALSE;
  BOOLEAN            IsError = !NT_SUCCESS( Status );

  SelfStatus = Self->Irp->IoStatus.Status;

  // Reference counter is unconditionally decremented because it is also unconditionally
  // incremented when enqueuing.
  InterlockedDecrement( &Ias->Mq->RefCount );

  if ( IsTX ) {
    if ( !IsError ) {
      InterlockedIncrement( &Ias->Mq->Counters.NumSent );
      InterlockedAdd( &Ias->Mq->Counters.TxBytes, LONG( Self->Wsb.Length ) );
      // Don't need synchronization to set this field as it's only really used for
      // bookkeeping and tracking disconnections.
      Ias->Mq->LastSendTimestamp = Timestamp( );
    }

    Ias->Mq->CbPostSend( Self, Ias->Mq->UserContext, Status );

    Self->SetIsIdle( );
    //  Send completed. Return to the freelist.
    Ias->Mq->TxFreeListPush( Self );
  } else {
    if ( !IsError ) {
      // Received a message. Send it to the RX queue for processing later.
      Self->SetIsWaiting( );
      InterlockedIncrement( &Ias->Mq->Counters.NumReceived );
      InterlockedAdd( &Ias->Mq->Counters.RxBytes, LONG( Self->Wsb.Length ) );
      Ias->Mq->RxPush( Self );
    } else {
      // Error receiving message. Return to the freelist.
      Self->SetIsIdle( );
      Ias->Mq->RxFreeListPush( Self );
    }
  }

  return STATUS_MORE_PROCESSING_REQUIRED;
}

/*
    Returns whether it is possible to write data into the buffer message.
*/
_Success_( return != false ) _Must_inspect_result_ bool ASYNC_MSG_QUEUE::CanBuffer( ) {
  return ( BufferMessage || ( BufferMessage = TxFreeListPop( ) ) );
}

/*
    Setup this message queue with the provided `Context`.

    This function is responsible for allocating all internal queue resources, such as IRPs
    and pre-allocated messages.

    @param Context User information required for internal functionality such as callbacks.
*/
_Success_( return != false ) _Must_inspect_result_
    bool ASYNC_MSG_QUEUE::Setup( MSG_QUEUE_SETUP_CONTEXT& Context ) {
  PASYNC_MSG   Msg{ };
  USHORT       QueueDepth{ };
  PSLIST_ENTRY Entry{ };
  PSLIST_ENTRY Next{ };
  KEVENT       DisconnectionEvent{ };
  PIRP         DisconnectionIrp{ };
  NTSTATUS     Status = STATUS_UNSUCCESSFUL;
  UINT8*       QueueName = REFLECT( ASYNC_MSG_TYPE::TX );

  UserContext = Context.UserContext;
  CbPreSend = Context.CbPreSend;
  CbPostSend = Context.CbPostSend;
  CbOnReceive = Context.CbOnReceive;

  /* TODO: Get the socket creation code out of the other project and insert it here. */

  dbg( "Allocating IRP for reconnection operations.\n" );
  NetOpIrp = IoAllocateIrp( 1, false );

  if ( !NetOpIrp ) {
    dbg( "Unable to allocate IRP for reconnection.\n" );
    goto Failure;
  }

  dbg( "Allocating %lu messages of %u bytes each for '%s'.\n",
       ASYNC_MSG_QUEUE_SIZE,
       ASYNC_MSG_SIZE,
       QueueName );

  for ( UINT32 Index = 0u; Index < ASYNC_MSG_QUEUE_SIZE; Index++ ) {
    Msg = ( PASYNC_MSG )( ExAllocatePool2(
        POOL_FLAG_NON_PAGED, sizeof( ASYNC_MSG ), MSG_POOL_TAG_RX
    ) );

    if ( !Msg ) {
      goto Failure;
    }

    memset( Msg, 0, sizeof( *Msg ) );

    /* Setup the message and establish the Queue <-> Message relationship. */
    if ( !Msg->Setup( this, ASYNC_MSG_TYPE::TX ) ) {
      dbg( "Unable to initialize message %#016llx of type '%s'.\n", Msg, QueueName );
      goto Failure;
    }

    TxFreeListPush( Msg );
  }

  QueueName = ( UINT8* )( REFLECT( ASYNC_MSG_TYPE::RX ) );

  dbg( "Allocating %lu messages of %u bytes each for '%s'.\n",
       ASYNC_MSG_QUEUE_SIZE,
       ASYNC_MSG_SIZE,
       QueueName );

  for ( UINT32 Index = 0u; Index < ASYNC_MSG_QUEUE_SIZE; Index++ ) {
    Msg = ( PASYNC_MSG )( ExAllocatePool2(
        POOL_FLAG_NON_PAGED, sizeof( ASYNC_MSG ), MSG_POOL_TAG_RX
    ) );
    if ( !Msg ) {
#if _DEBUG
      dbg( "Unable to allocate async message for queue.\n" );
      __debugbreak( );
#endif
      goto Failure;
    }

    memset( Msg, 0, sizeof( *Msg ) );

    if ( !Msg->Setup( this, ASYNC_MSG_TYPE::RX ) ) {
      goto Failure;
    }

    RxFreeListPush( Msg );
  }

  /* Leave an outstanding message ready for buffering. */
  BufferMessage = TxFreeListPop( );

  /* Default to connected since that's the state we receive it in from the loader. */
  SockState = SOCKET_STATE::CONNECTED;

  /* Assume it is connected by default since we're being called by the loader. */
  QueueState = ASYNC_MSG_QUEUE_STATE::Initialized;

  return true;

Failure:
  dbg( "Message queue initialization failed. Undoing all allocations.\n" );

  QueueDepth = ExQueryDepthSList( &TxFreeList );
  Entry = ExpInterlockedFlushSList( &TxFreeList );

  dbg( "Releasing %u messages from '%s'.\n", QueueDepth, REFLECT( TxFreeList ) );

  while ( Entry ) {
    Next = Entry->Next;

    dbg( "Releasing message %#016llx.\n", ( ULONG_PTR )Entry );
    Msg = ( PASYNC_MSG )( Entry );
    Msg->Rundown( );
    memset( Msg, 0, sizeof( *Msg ) );
    ExFreePoolWithTag( Msg, MSG_POOL_TAG_TX );

    Entry = Next;
  }

  Msg = nullptr;
  QueueDepth = ExQueryDepthSList( &RxFreeList );
  Entry = ExpInterlockedFlushSList( &RxFreeList );
  dbg( "Releasing %u messages from '%s'.\n", QueueDepth, REFLECT( RxFreeList ) );

  while ( Entry ) {
    Next = Entry->Next;

    dbg( "Releasing message %#016llx.\n", ( ULONG_PTR )Entry );
    Msg = ( PASYNC_MSG )( Entry );
    Msg->Rundown( );
    memset( Msg, 0, sizeof( *Msg ) );
    ExFreePoolWithTag( Msg, MSG_POOL_TAG_RX );

    Entry = Next;
  }

  if ( NetOpIrp ) {
    IoFreeIrp( NetOpIrp );
    NetOpIrp = nullptr;
  }

  QueueState = ASYNC_MSG_QUEUE_STATE::Uninitialized;

  /* Disconnect the socket */
  DisconnectionIrp = IoAllocateIrp( 1, false );
  KeInitializeEvent( &DisconnectionEvent, NotificationEvent, false );
  IoSetCompletionRoutine(
      DisconnectionIrp,
      []( PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context ) -> NTSTATUS {
        UNREFERENCED_PARAMETER( DeviceObject );
        UNREFERENCED_PARAMETER( Irp );
        KeSetEvent( ( PRKEVENT )Context, 0, false );
        return STATUS_MORE_PROCESSING_REQUIRED;
      },
      &DisconnectionEvent,
      true,
      true,
      true
  );

  Status = Tcp->WskDisconnect( Socket, nullptr, WSK_FLAG_ABORTIVE, DisconnectionIrp );

  if ( Status == STATUS_PENDING ) {
    dbg( "Waiting for disconnection to complete.\n" );
    KeWaitForSingleObject( &DisconnectionEvent, Executive, KernelMode, false, nullptr );
    dbg( "Disconnected.\n" );
  }

  IoFreeIrp( DisconnectionIrp );

  Socket = nullptr;
  Tcp = nullptr;
  Npi.Client = nullptr;
  Npi.Dispatch = nullptr;
  RemoteAddr = nullptr;

  return false;
}

_IRQL_requires_max_( DISPATCH_LEVEL ) VOID ASYNC_MSG_QUEUE::SendBufferedMsg( ) {
  if ( AsyncSend( PASYNC_MSG( BufferMessage ) ) ) {
    // Message was sent or at least enqueued. For us, this means the Windows I/O subsystem
    // has taken ownership of the message. Once the IRP completes it will be returned to
    // its respective queue.
    BufferMessage = nullptr;
    ASSERTMSG( "BufferMessage != NULL", BufferMessage != NULL );
  }

  /*
    A message has not been successfully transmitted by WSK in over `SOCK_SEND_TIMEOUT`
    milliseconds. Disconnect and reconnect to the server.
  */
  if ( MsSinceLastSend( ) >= SOCK_SEND_TIMEOUT ) {
    AsyncDisconnect( );
  }
}

_IRQL_requires_max_( DISPATCH_LEVEL ) VOID ASYNC_MSG_QUEUE::AsyncDisconnect( ) {
  NTSTATUS Status{ };

  if ( SockState == SOCKET_STATE::DISCONNECTED ||
       SockState == SOCKET_STATE::DISCONNECTING )
    return;

  SetSockState( SOCKET_STATE::DISCONNECTING );
  IoReuseIrp( NetOpIrp, STATUS_UNSUCCESSFUL );
  IoSetCompletionRoutine(
      NetOpIrp,
      []( PDEVICE_OBJECT DevObj, PIRP Irp, PVOID Context ) -> NTSTATUS {
        UNREFERENCED_PARAMETER( DevObj );
        UNREFERENCED_PARAMETER( Irp );

        PASYNC_MSG_QUEUE Self = ( PASYNC_MSG_QUEUE )( Context );

        // Set to disconnected regardless of IRP status because if `WskCloseSocket`
        // somehow errors out we are in an undefined state and it's better to clean up and
        // reconnect.
        Self->SetSockState( SOCKET_STATE::DISCONNECTED );

        return STATUS_MORE_PROCESSING_REQUIRED;
      },
      this,
      true,
      true,
      true
  );

  Status = Tcp->Basic.WskCloseSocket( Socket, NetOpIrp );

  if ( Status != STATUS_PENDING && Status != STATUS_SUCCESS ) {
#ifdef _DEBUG
    __debugbreak( );
#endif
  }
}

_IRQL_requires_max_( DISPATCH_LEVEL ) bool ASYNC_MSG_QUEUE::AsyncReconnect( ) {
  NTSTATUS Status = STATUS_UNSUCCESSFUL;
  UINT32   Pending{ };

  static SOCKADDR_IN LocalAddr{ };

  LocalAddr.sin_family = AF_INET;
  LocalAddr.sin_port = 0;
  LocalAddr.sin_addr.S_un.S_addr = INADDR_ANY;

  if ( SockState != SOCKET_STATE::DISCONNECTED ) {
#ifdef _DEBUG
    __debugbreak( );
#endif
    return false;
  }

  // We don't want to mess with any pointers (e.g., Tcp or Socket) with messages
  // in-transit..
  Pending = InterlockedOr( &RefCount, 0 );

  if ( Pending != 0 ) {
    return false;
  }

  // With all asynchronous operations cancelled/completed (i.e., RefCount is 0) we can now
  // proceed without concern for race conditions since the single-thread main loop will be
  // responsible just waiting on the I/O subsystem to set the socket state.
  IoReuseIrp( NetOpIrp, STATUS_UNSUCCESSFUL );
  IoSetCompletionRoutine(
      NetOpIrp,
      []( PDEVICE_OBJECT DevObj, PIRP Irp, PVOID Context ) -> NTSTATUS {
        UNREFERENCED_PARAMETER( DevObj );
        PASYNC_MSG_QUEUE Self = ( PASYNC_MSG_QUEUE )( Context );
        NTSTATUS         Status = Irp->IoStatus.Status;

        if ( Status == STATUS_SUCCESS ) {
          Self->SetSockState(
              SOCKET_STATE::CONNECTED, ( PVOID )Irp->IoStatus.Information
          );
        } else {
          // We set to `DISCONNECTED` instead of `DISCONNECTING` because this path is only
          // ever reached after a full, abortive disconnection has completed. Furthermore,
          // this causes internal state to be reset.
          Self->SetSockState( SOCKET_STATE::DISCONNECTED );
        }

        return STATUS_MORE_PROCESSING_REQUIRED;
      },
      this,
      true,
      true,
      true
  );

  SetSockState( SOCKET_STATE::CONNECTING );
  Status = Npi.Dispatch->WskSocketConnect(
      Npi.Client,
      SOCK_STREAM,
      IPPROTO_TCP,
      ( PSOCKADDR )&LocalAddr,
      RemoteAddr->ai_addr,
      0,
      this,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      NetOpIrp
  );

  if ( Status != STATUS_PENDING && Status != STATUS_SUCCESS ) {
    AsyncDisconnect( );
#ifdef _DEBUG
    dbg( "Attempt to reconnect socket failed: %#08lx\n", Status );
    __debugbreak( );
#endif
  }

  return true;
}

_IRQL_requires_max_( DISPATCH_LEVEL ) UINT64 ASYNC_MSG_QUEUE::MsSinceLastSend( ) const {
  return ( Timestamp( ) - max( 1, LastSendTimestamp ) ) / 10'000;
}

/*
    Note: It is the callers responsibility to properly initialize the message's IRP
   completion routine.
*/
_IRQL_requires_max_( DISPATCH_LEVEL ) bool ASYNC_MSG_QUEUE::AsyncSend(
    _In_ PASYNC_MSG Msg
) {
  ASSERTMSG( "Msg != nullptr", Msg != nullptr );

  if ( !Msg ) return false;

  NTSTATUS Status = STATUS_UNSUCCESSFUL;

  ASSERT( CbPreSend != nullptr );

  CbPreSend( Msg, UserContext );

  Status = Tcp->WskSend( Socket, &Msg->Wsb, WSK_FLAG_NODELAY, Msg->Irp );

  switch ( Status ) {
  case STATUS_SUCCESS:
  case STATUS_PENDING: {
    InterlockedIncrement( &RefCount );
    break;
  }
  case STATUS_CONNECTION_RESET:
  case STATUS_CONNECTION_DISCONNECTED:
    return false;
  }

  return Status == STATUS_PENDING;
}

_IRQL_requires_max_( DISPATCH_LEVEL ) VOID ASYNC_MSG_QUEUE::AsyncRecv( ) {
  NTSTATUS     Status = STATUS_UNSUCCESSFUL;
  PASYNC_MSG   Msg{ };
  PSLIST_ENTRY Head{ };
  PSLIST_ENTRY Tail{ };
  PSLIST_ENTRY Next{ };

  Tail = ExpInterlockedFlushSList( &Rx );

  if ( Tail ) {
    // Reverse to keep FIFO order.
    while ( Tail != NULL ) {
      Next = Tail->Next;
      Tail->Next = Head;
      Head = Tail;
      Tail = Next;
    }

    while ( Head ) {
      Msg = ( PASYNC_MSG )( Head );

      ASSERT( CbOnReceive != nullptr );

      CbOnReceive( Msg );

      Head = Head->Next;
    }
  }

  /*
     Replenish outstanding buffers.
     We always leave them pending so that the kernel can write data as soon as it arrives.
  */
  for ( ;; ) {
    if ( !Tcp || !Socket ) break;

    Msg = RxFreeListPop( );
    if ( !Msg ) break;

    Msg->Recycle( );
    Status = Tcp->WskReceive( Socket, &Msg->Wsb, 0, Msg->Irp );
    ASSERTMSG(
        "Queue thinks socket is connected but WskReceive failed.",
        Status == STATUS_PENDING
    );
    if ( Status != STATUS_PENDING ) {
#ifdef _DEBUG
      dbg( "Queue thinks socket is connected but WskReceive failed.\n" );
      __debugbreak( );
#endif
      // Return the message to the queue.
      RxFreeListPush( Msg );
    } else {
      InterlockedIncrement( &RefCount );
    }
  }
}

_IRQL_requires_max_(
    DISPATCH_LEVEL
) VOID ASYNC_MSG_QUEUE::SetSockState( SOCKET_STATE NewState, VOID* Sock ) {
  // Transition the state machine from the current state to the next (S_c => S_n).

  if ( NewState == SOCKET_STATE::CONNECTED ) {
    Socket = ( PWSK_SOCKET )Sock;
    ASSERT( Socket != nullptr );
    Tcp = ( PWSK_PROVIDER_STREAM_DISPATCH )( Socket->Dispatch );
    ASSERT( Tcp != nullptr );
    ReconnectionAttempts = 0;
    LastSendTimestamp = 0LLU;
  }

  if ( NewState == SOCKET_STATE::DISCONNECTED ) {
    Socket = nullptr;
    Tcp = nullptr;
    // Cap the value to 5 to prevent unbound left-shifting down below.
    ReconnectionAttempts = min( ReconnectionAttempts + 1, 5 );

    // Schedule the next reconnection time to T_now + min( ( 1 * 2^R ), 35sec )
    // Where R is the number of reconnection attempts, with the maximum delay between
    // attempts set to 35 seconds.
    // Analysis:
    // min( 1 * 2^0, 35sec )     => 1000  (1s )
    // min( 1 * 2^1, 35sec )     => 2000  (2s )
    // min( 1 * 2^2, 35sec )     => 4000  (4s )
    // min( 1 * 2^3, 35sec )     => 8000  (8s )
    // min( 1 * 2^4, 35sec )     => 16000 (16s)
    // min( 1 * 2^5, 35sec )     => 32000 (32s)
    // min( 1 * 2^6, 35sec )     => 35000 (35s)
    // min( 1 * 2^N, 35sec )     => 35000 for every N > 5.

    ULONG64 Jitter = UnsafeRangeRng( 50, 300 ) * 10'000;

    // Baseline is 5 sec.
    ULONG64 Delay =
        min( 5'000llu * ( 1llu << ReconnectionAttempts ), MAX_RECONNECTION_DELAY ) *
        10'000;

    NextReconnectTime = Timestamp( ) + ( Jitter + Delay );
  }

  // Update the state last.
  SockState = NewState;
}

BOOLEAN ASYNC_MSG_QUEUE::IsTxFreeListEmpty( ) {
  return ExQueryDepthSList( &TxFreeList ) == 0;
}

VOID ASYNC_MSG_QUEUE::TxFreeListPush( PASYNC_MSG Msg ) {
  ASSERT( Msg != nullptr );
  ExpInterlockedPushEntrySList( &TxFreeList, PSLIST_ENTRY( Msg ) );
}

PASYNC_MSG ASYNC_MSG_QUEUE::TxFreeListPop( ) {
  PASYNC_MSG Msg = PASYNC_MSG( ExpInterlockedPopEntrySList( &TxFreeList ) );
  if ( !Msg ) return nullptr;
  Msg->Prepare( 0, 0 );
  memset( Msg->Data, 0, ASYNC_MSG_SIZE );
  return Msg;
}

VOID ASYNC_MSG_QUEUE::RxFreeListPush( PASYNC_MSG Msg ) {
  ASSERT( Msg != nullptr );
  ExpInterlockedPushEntrySList( &RxFreeList, PSLIST_ENTRY( Msg ) );
}

PASYNC_MSG ASYNC_MSG_QUEUE::RxFreeListPop( ) {
  PASYNC_MSG Msg = PASYNC_MSG( ExpInterlockedPopEntrySList( &RxFreeList ) );
  if ( !Msg ) return nullptr;
  Msg->Prepare( 0, 0 );
  memset( Msg->Data, 0, ASYNC_MSG_SIZE );
  return Msg;
}

VOID ASYNC_MSG_QUEUE::RxPush( PASYNC_MSG Msg ) {
  ASSERT( Msg != nullptr );
  ExpInterlockedPushEntrySList( &Rx, PSLIST_ENTRY( Msg ) );
}

VOID ASYNC_MSG_QUEUE::TrySendBuffered( ) {
  constexpr UINT64 MinimumDelay = 8llu;
  constexpr UINT64 MaximumBufferedSize = 512u;

  // In order to ensure good throughput and less delay before running the other functions,
  // we only send a buffered packet after `MinimumDelay` ms elapsed since the last packet
  // or there are over 512 bytes buffered up.
  UINT64     CurrentTimestamp = Timestamp( );
  UINT64     ElapsedMs{ };
  PASYNC_MSG AsMsg = ( PASYNC_MSG )( BufferMessage );
  SIZE_T     Length = AsMsg->Wsb.Length;

  ASSERT( Length < ASYNC_MSG_SIZE );
  ASSERT( Length != 0 );

  if ( Length == 0 ) return;

  if ( Length > ASYNC_MSG_SIZE ) {
#ifdef _DEBUG
    __debugbreak( );
#endif
    return;
  }

  if ( !LastSendTimestamp ) {
    // This is the first packet we're sending. Send it now if possible.
    dbg( "First attempt to send buffered message. Setting initial timestamp.\n" );
    LastSendTimestamp = Timestamp( );
  }

  ElapsedMs = max( 1, CurrentTimestamp - LastSendTimestamp ) / 10'000llu;

  if ( ElapsedMs >= MinimumDelay || Length >= MaximumBufferedSize ) {
    SendBufferedMsg( );
  }
}

VOID ASYNC_MSG_QUEUE::TryFlushBuffered( ) const {
  if ( auto Msg = ( PASYNC_MSG )( BufferMessage ); Msg->HasPendingData( ) ) {
    Msg->Recycle( );
  }
}

_Success_( return != false ) _Must_inspect_result_
    bool ASYNC_MSG::Setup( _In_ PASYNC_MSG_QUEUE Queue, _In_ ASYNC_MSG_TYPE MsgType ) {
  auto const PoolTag = MsgType == ASYNC_MSG_TYPE::TX ? MSG_POOL_TAG_TX : MSG_POOL_TAG_RX;
  Data = ( UINT8* )ExAllocatePool2( POOL_FLAG_NON_PAGED, ASYNC_MSG_SIZE, PoolTag );

  if ( !Data ) return false;

  Mdl = IoAllocateMdl( Data, ASYNC_MSG_SIZE, false, false, nullptr );
  Irp = IoAllocateIrp( 1, false );

  if ( !Mdl || !Irp ) {
    if ( Mdl ) IoFreeMdl( Mdl );
    if ( Irp ) IoFreeIrp( Irp );

    ExFreePoolWithTag( Data, PoolTag );

    return false;
  }

  __try {
    MmProbeAndLockPages( Mdl, KernelMode, IoModifyAccess );
  } __except ( EXCEPTION_EXECUTE_HANDLER ) {
    IoFreeMdl( Mdl );
    IoFreeIrp( Irp );
    ExFreePoolWithTag( Data, PoolTag );
    reurn false;
  }

  Wsb.Mdl = Mdl;
  Wsb.Length = 0;
  Wsb.Offset = 0;

  memset( Data, 0, ASYNC_MSG_SIZE );

  Ias.Mq = Queue;
  Ias.Message = this;
  Type_ = MsgType;
  State_ = ASYNC_MSG_STATE::Idle;

  return true;
}

VOID ASYNC_MSG::Rundown( ) {
  IoFreeIrp( Irp );
  MmUnlockPages( Mdl );
  IoFreeMdl( Mdl );

  memset( Data, 0, ASYNC_MSG_SIZE );
  ExFreePoolWithTag( Data, ' ' );

  Irp = nullptr;
  Mdl = nullptr;
  Data = nullptr;

  memset( &Ias, 0x00, sizeof( Ias ) );
  memset( &Wsb, 0x00, sizeof( Wsb ) );
}

VOID ASYNC_MSG::Recycle( ) {
  memset( Data, 0, ASYNC_MSG_SIZE );
  Wsb.Length = 0;
  Wsb.Offset = 0;
  IoSetCompletionRoutine( Irp, GenericIrpCompletion, this, true, true, true );
  IoReuseIrp( Irp, STATUS_UNSUCCESSFUL );
}

VOID ASYNC_MSG::Prepare( _In_ UINT32 Offset, _In_ UINT32 Length ) {
  IoCancelIrp( Irp );
  IoReuseIrp( Irp, STATUS_UNSUCCESSFUL );
  IoSetCompletionRoutine( Irp, GenericIrpCompletion, this, true, true, true );
  Wsb.Length = min( Length, ASYNC_MSG_SIZE );
  Wsb.Offset = min( Offset, ASYNC_MSG_SIZE );
}

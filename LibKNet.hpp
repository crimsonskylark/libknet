#pragma once

#include <ntifs.h>
#include <sal.h>
#include <wsk.h>

#ifdef _DEBUG
#define b2s( c ) ( c ) ? "true" : "false"
#define dbg( ... )                                                                       \
  do {                                                                                   \
    const auto _PREFIX = "[%s:%u] ";                                                     \
    DbgPrintEx(                                                                          \
        DPFLTR_IHVDRIVER_ID,                                                             \
        DPFLTR_MASK | DPFLTR_INFO_LEVEL,                                                 \
        _PREFIX,                                                                         \
        __FUNCTION__,                                                                    \
        __LINE__                                                                         \
    );                                                                                   \
    DbgPrintEx( DPFLTR_IHVDRIVER_ID, DPFLTR_MASK | DPFLTR_INFO_LEVEL, __VA_ARGS__ );     \
  } while ( 0 );

#define dbg_no_prefix( ... )                                                             \
  DbgPrintEx( DPFLTR_IHVDRIVER_ID, DPFLTR_MASK | DPFLTR_INFO_LEVEL, __VA_ARGS__ );
#else
#define b2s( c )
#define dbg( ... )
#endif

/*
    Number of messages pre-allocated into the message queue.
*/
inline constexpr auto ASYNC_MSG_QUEUE_SIZE = 64ULL;

/*
    Size of every pre-allocated message.
*/
inline constexpr auto ASYNC_MSG_SIZE = 1024ULL;

/*
    How long to wait (in milliseconds) before disconnecting the socket.
*/
inline constexpr auto SOCK_SEND_TIMEOUT = 15'000u;

/*
    Maximum reconnection delay is 35s.
*/
constexpr auto MAX_RECONNECTION_DELAY = 35'000llu;

constexpr auto MSG_POOL_TAG_TX = 'XTNK';
constexpr auto MSG_POOL_TAG_RX = 'XRNK';

struct ASYNC_MSG;
struct ASYNC_MSG_QUEUE;
struct IRP_ASYNC_CONTEXT;
struct PACKET_HEADER;
struct PLAYER_DATA;

using PASYNC_MSG_QUEUE = ASYNC_MSG_QUEUE*;
using PASYNC_MSG = ASYNC_MSG*;
using PIRP_ASYNC_CONTEXT = IRP_ASYNC_CONTEXT*;

/*
    Invoked by the message queue before a message is sent over the wire. This is where
    encryption should be applied.

    @param Msg The message to send over the network
    @param Context User-registered context.
*/
using MSG_PRE_SEND_CALLBACK = void ( * )( PASYNC_MSG Msg, PVOID Context );

/*
    Invoked by the message queue after a message is sent over the wire. This callback is
    mostly useful for bookkeeping purposes.

    @param Msg Message that may have been sent over the network.
    @param Context User-registered context.
    @param Status Status returned by the kernel network subsystem. If the message was
    successfully sent, this is `STATUS_SUCCESS`.
*/
using MSG_POST_SEND_CALLBACK = _IRQL_always_function_max_(
    DISPATCH_LEVEL
) void ( * )( PASYNC_MSG Msg, PVOID Context, NTSTATUS Status );

/*
    Invoked once by the message queue for every messaged received on the socket.

    @param Msg Raw, unprocessed bytestream received from the network
*/
using MSG_PRE_RECEIVE_CALLBACK =
    _IRQL_requires_max_( DISPATCH_LEVEL ) VOID ( * )( PASYNC_MSG Msg );

struct IRP_ASYNC_CONTEXT {
  PASYNC_MSG_QUEUE Mq;
  PASYNC_MSG       Message;
};

enum class ASYNC_MSG_STATE : UINT8 { Idle = 1, Pending, Waiting };
enum class ASYNC_MSG_TYPE : UINT8 { TX = 1, RX };
enum class ASYNC_MSG_QUEUE_STATE : UINT8 { Uninitialized, Initialized };
enum class SOCKET_STATE : UINT8 { DISCONNECTED, DISCONNECTING, CONNECTING, CONNECTED };

struct MSG_QUEUE_SETUP_CONTEXT {
  PVOID                    UserContext{ };
  MSG_PRE_SEND_CALLBACK    CbPreSend{ };
  MSG_POST_SEND_CALLBACK   CbPostSend{ };
  MSG_PRE_RECEIVE_CALLBACK CbOnReceive{ };
};

struct ASYNC_MSG_QUEUE {
  _Interlocked_ SLIST_HEADER    Rx{ };
  _Interlocked_ SLIST_HEADER    RxFreeList{ };
  _Interlocked_ SLIST_HEADER    TxFreeList{ };
  WSK_PROVIDER_NPI              Npi{ };
  PVOID                         UserContext{ };
  MSG_PRE_SEND_CALLBACK         CbPreSend{ };
  MSG_POST_SEND_CALLBACK        CbPostSend{ };
  MSG_PRE_RECEIVE_CALLBACK      CbOnReceive{ };
  PADDRINFOEXW                  RemoteAddr{ };
  PIRP                          NetOpIrp{ };
  UINT64                        LastSendTimestamp{ };
  UINT64                        NextReconnectTime{ };
  PWSK_SOCKET                   Socket{ };
  PWSK_PROVIDER_STREAM_DISPATCH Tcp{ };
  PVOID                         BufferMessage{ };
  _Interlocked_ volatile LONG   RefCount{ };
  UINT32                        ReconnectionAttempts{ };
  SOCKET_STATE                  SockState{ SOCKET_STATE::CONNECTED };
  ASYNC_MSG_QUEUE_STATE         QueueState{ ASYNC_MSG_QUEUE_STATE::Uninitialized };

  struct {
    volatile LONG NumSent;
    volatile LONG TxBytes;
    volatile LONG NumReceived;
    volatile LONG RxBytes;
  } Counters;

  _Success_( return != false ) _Must_inspect_result_ bool CanBuffer( );
  _Success_( return != false ) bool IsConnected( ) const {
    return SockState == SOCKET_STATE::CONNECTED;
  }
  _Success_( return != false ) _Must_inspect_result_
      bool Setup( MSG_QUEUE_SETUP_CONTEXT& Context );
  _IRQL_requires_max_( DISPATCH_LEVEL ) VOID SendBufferedMsg( );
  _IRQL_requires_max_( DISPATCH_LEVEL ) VOID AsyncDisconnect( );
  _IRQL_requires_max_( DISPATCH_LEVEL ) bool AsyncReconnect( );
  _IRQL_requires_max_( DISPATCH_LEVEL ) UINT64 MsSinceLastSend( ) const;
  _IRQL_requires_max_( DISPATCH_LEVEL ) bool AsyncSend( _In_ PASYNC_MSG Msg );
  _IRQL_requires_max_( DISPATCH_LEVEL ) VOID AsyncRecv( );
  _IRQL_requires_max_(
      DISPATCH_LEVEL
  ) VOID SetSockState( SOCKET_STATE NewState, VOID* Sock = nullptr );
  BOOLEAN    IsTxFreeListEmpty( );
  VOID       TxFreeListPush( PASYNC_MSG Msg );
  PASYNC_MSG TxFreeListPop( );
  VOID       RxFreeListPush( PASYNC_MSG Msg );
  PASYNC_MSG RxFreeListPop( );
  VOID       RxPush( PASYNC_MSG Msg );
  VOID       TrySendBuffered( );
  VOID       TryFlushBuffered( ) const;
  VOID       ResetTimers( ) {
    LastSendTimestamp = 0llu;
    NextReconnectTime = 0llu;
  }
};

_declspec( align( alignof( SLIST_ENTRY ) ) ) struct ASYNC_MSG {
  SLIST_ENTRY       Entry{ };
  WSK_BUF           Wsb{ };
  IRP_ASYNC_CONTEXT Ias{ };
  PMDL              Mdl{ };
  PIRP              Irp{ };
  PUINT8            Data{ };

  _Success_( return != false ) _Must_inspect_result_
      bool Setup( _In_ PASYNC_MSG_QUEUE Queue, _In_ ASYNC_MSG_TYPE MsgType );
  VOID                 Rundown( );
  VOID                 Recycle( );
  VOID                 Prepare( _In_ UINT32 Offset = 0, _In_ UINT32 Length = 0 );
  __forceinline SIZE_T Remainder( ) const {
    // Let
    //    R = Remainder in bytes
    //    M = Max size in bytes
    //    C = Current size in bytes
    // The amount of free space available for writing is given as R = M - C
    return ASYNC_MSG_SIZE - min( Wsb.Length, ASYNC_MSG_SIZE );
  }
  __forceinline PUINT8 End( ) const { return ( PUINT8 )( Data + ASYNC_MSG_SIZE ); }
  __forceinline bool   CanFit( SIZE_T Length ) const { return Remainder( ) > Length; }
  __forceinline PUINT8 CurrentHeader( ) const {
    // Offset is how far into the data buffer we are, in bytes.
    SIZE_T Offset = Wsb.Length;
    // New packet starts immediately after the previous.
    return ( PUINT8 )( Data + Offset );
  }
  __forceinline PUINT8 CurrentData( ) const { return ( PUINT8 )( CurrentHeader( ) + 1 ); }

  bool IsIdle( ) const { return State_ == ASYNC_MSG_STATE::Idle; }
  bool IsPending( ) const { return State_ == ASYNC_MSG_STATE::Pending; }
  bool IsWaiting( ) const { return State_ == ASYNC_MSG_STATE::Waiting; }
  bool HasPendingData( ) const { return Wsb.Length != 0; }

  ASYNC_MSG_TYPE Type( ) const { return Type_; }
  bool           IsRX( ) const { return Type( ) == ASYNC_MSG_TYPE::RX; }
  bool           IsTX( ) const { return Type( ) == ASYNC_MSG_TYPE::TX; }

  VOID SetIsIdle( ) { State_ = ASYNC_MSG_STATE::Idle; }
  VOID SetIsWaiting( ) { State_ = ASYNC_MSG_STATE::Waiting; }
  VOID SetIsPending( ) { State_ = ASYNC_MSG_STATE::Pending; }

  SIZE_T Count( ) const { return Wsb.Length; }

private:
  ASYNC_MSG_STATE State_{ };
  ASYNC_MSG_TYPE  Type_{ };
};

static_assert( alignof( ASYNC_MSG ) == alignof( SLIST_ENTRY ) );

inline PASYNC_MSG_QUEUE MsgQueue( ) {
  static ASYNC_MSG_QUEUE _Mq{ };
  return &_Mq;
}
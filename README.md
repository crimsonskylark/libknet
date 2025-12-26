# libknet

Experimental library for Windows kernel-level networking.

Warning: This library was extracted from a larger (equally experimental) project and cleaned/modified up for public release. It has not been as extensively tested as the original implementation and may contain bugs. Review before usage.

It is also missing the socket creation logic. Either add it manually or simply set the required fields in `ASYNC_MSG_QUEUE` as required: `Tcp`, `Socket`, `Npi` and `RemoteAddr`.

Note the library takes ownership of the socket and it will assume it is its only user. Do not share the socket with other routines or objects.

---

MIT License

Copyright (c) 2023 crimsonskylark

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

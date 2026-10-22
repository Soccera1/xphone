# Third-party dependencies and references

XPhone links system-provided X.Org libraries, Fontconfig, PulseAudio, Opus,
SpeexDSP, OpenSSL, libevent, Jansson, nghttp2, nghttp3, and ngtcp2. Their licenses
remain with their respective projects; no dependency sources are vendored here.

The QUIC/HTTP/3 integration was developed with reference to ngtcp2 v1.25.0's
official client/server examples, particularly TLS session setup, callback
wiring, stream flow control, and payload buffer lifetime management:
<https://github.com/ngtcp2/ngtcp2/tree/v1.25.0/examples>.

The upstream examples carry the following MIT license:

Copyright (c) 2025 ngtcp2 contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.

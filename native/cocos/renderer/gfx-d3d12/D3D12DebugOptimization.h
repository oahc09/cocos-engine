/****************************************************************************
 Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.

 http://www.cocos.com

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights to
 use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 of the Software, and to permit persons to whom the Software is furnished to do so,
 subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
****************************************************************************/

#pragma once

// Verbose backend tracing is compiled out by default, including argument
// evaluation. Enable it explicitly for focused backend profiling sessions.
#if defined(CC_D3D12_ENABLE_DIAGNOSTIC_LOGS) && CC_D3D12_ENABLE_DIAGNOSTIC_LOGS
    #define CC_D3D12_DIAGNOSTICS_ENABLED 1
    #define CC_D3D12_DIAGNOSTIC_LOG(...) CC_LOG_INFO(__VA_ARGS__)
#else
    #define CC_D3D12_DIAGNOSTICS_ENABLED 0
    #define CC_D3D12_DIAGNOSTIC_LOG(...) ((void)0)
#endif

#if defined(_MSC_VER) && !defined(NDEBUG)
// D3D12 command recording is CPU-bound in draw-heavy Debug builds. Preserve
// debug symbols and assertions while avoiding /Od and /RTC overhead here.
#pragma runtime_checks("su", off)
#pragma optimize("gt", on)
#endif

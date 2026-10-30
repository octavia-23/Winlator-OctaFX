// Copyright  © 2023 Advanced Micro Devices, Inc.
// Copyright  © 2024-2026 Arm Limited.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "ffxm_assert.h"
#include <stdlib.h>  // for malloc()

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>  // required for OutputDebugString()
#include <stdio.h>    // required for sprintf_s
#elif defined(__ANDROID__)
#include <android/log.h>  // required for __android_log_print()
#include <stdio.h>        // required for snprintf
#else
#include <stdio.h>        // required for snprintf
#endif                // #ifndef _WIN32

#if defined(__ANDROID__)
#ifndef FFXM_ANDROID_LOG_TAG
#define FFXM_ANDROID_LOG_TAG "ArmASR"
#endif
#endif

namespace arm
{

static FfxmAssertCallback s_assertCallback;

// set the printing callback function
void ffxmAssertSetPrintingCallback(FfxmAssertCallback callback)
{
    s_assertCallback = callback;
    return;
}

// implementation of assert reporting
bool ffxmAssertReport(const char* file, int32_t line, const char* condition, const char* message)
{
    if (!file) {

        return true;
    }

#ifdef _WIN32
    // form the final assertion string and output to the TTY.
    const size_t bufferSize = snprintf(NULL, 0, "%s(%d): ASSERTION FAILED. %s\n", file, line, message ? message : condition) + 1;
    char*        tempBuf    = (char*)malloc(bufferSize);
    if (!tempBuf) {

        return true;
    }

    if (!message) {
        sprintf_s(tempBuf, bufferSize, "%s(%d): ASSERTION FAILED. %s\n", file, line, condition);
    } else {
        sprintf_s(tempBuf, bufferSize, "%s(%d): ASSERTION FAILED. %s\n", file, line, message);
    }

    if (!s_assertCallback) {
        OutputDebugStringA(tempBuf);
    } else {
        s_assertCallback(tempBuf);
    }

    // free the buffer.
    free(tempBuf);

#else
    // form the final assertion string and route it somewhere visible.
    const char*  text       = message ? message : condition;
    const size_t bufferSize = (size_t)snprintf(NULL, 0, "%s(%d): ASSERTION FAILED. %s\n", file, line, text) + 1;
    char*        tempBuf    = (char*)malloc(bufferSize);
    if (!tempBuf) {

        return true;
    }

    snprintf(tempBuf, bufferSize, "%s(%d): ASSERTION FAILED. %s\n", file, line, text);

    if (!s_assertCallback) {
#if defined(__ANDROID__)
        // stderr is discarded on Android, so go through logcat instead.
        __android_log_print(ANDROID_LOG_ERROR, FFXM_ANDROID_LOG_TAG, "%s", tempBuf);
#else
        fputs(tempBuf, stderr);
#endif
    } else {
        s_assertCallback(tempBuf);
    }

    // free the buffer.
    free(tempBuf);
#endif

    return true;
}

} // namespace arm

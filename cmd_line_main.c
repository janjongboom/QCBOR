/*
 cmd_line_mainc.c -- basic tests for qcbor encoder / decoder
 
 This is governed by the MIT license.
 
 Copyright 2018 Laurence Lundblade
 
 Permission is hereby granted, free of charge, to any person obtaining
 a copy of this software and associated documentation files (the
 "Software"), to deal in the Software without restriction, including
 without limitation the rights to use, copy, modify, merge, publish,
 distribute, sublicense, and/or sell copies of the Software, and to
 permit persons to whom the Software is furnished to do so, subject to
 the following conditions:
 
 The above copyright notice and this permission notice shall be included
 in all copies or substantial portions of the Software.
 
 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 SOFTWARE.
 */

#include <stdio.h>

#include "basic_test.h"
#include "bstrwrap_tests.h"

int main(int argc, const char * argv[]) {

    printf("basic-test_one Result %d\n", basic_test_one());

    printf("cose_sign1_tbs_test %d\n", cose_sign1_tbs_test());

    printf("bstr_wrap_nest_test %d\n", bstr_wrap_nest_test());
    
    printf("bstr_wrap_error_test %d\n", bstr_wrap_error_test());
    
    printf("bstrwraptest %d\n", bstrwraptest());
    
    
    return 0;
}

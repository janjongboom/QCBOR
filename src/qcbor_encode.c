/*==============================================================================
 Copyright (c) 2016-2018, The Linux Foundation.
 Copyright (c) 2018, Laurence Lundblade.
 All rights reserved.
 
Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above
      copyright notice, this list of conditions and the following
      disclaimer in the documentation and/or other materials provided
      with the distribution.
    * Neither the name of The Linux Foundation nor the names of its
      contributors, nor the name "Laurence Lundblade" may be used to
      endorse or promote products derived from this software without
      specific prior written permission.
 
THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 ==============================================================================*/

/*===================================================================================
 FILE:  qcbor_encode.c
 
 DESCRIPTION:  This file contains the implementation of QCBOR.
 
 EDIT HISTORY FOR FILE:
 
 This section contains comments describing changes made to the module.
 Notice that changes are listed in reverse chronological order.
 
 when               who             what, where, why
 --------           ----            ---------------------------------------------------
 11/29/18           llundblade      Rework to simpler handling of tags and labels.
 11/9/18            llundblade      Error codes are now enums.
 11/1/18            llundblade      Floating support.
 10/31/18           llundblade      Switch to one license that is almost BSD-3.
 09/28/18           llundblade      Added bstr wrapping feature for COSE implementation.
 02/05/18           llundbla        Works on CPUs which require integer alignment. 
                                    Requires new version of UsefulBuf.
 07/05/17           llundbla        Add bstr wrapping of maps/arrays for COSE
 03/01/17           llundbla        More data types
 11/13/16           llundbla        Integrate most TZ changes back into github version.
 09/30/16           gkanike         Porting to TZ.
 03/15/16           llundbla        Initial Version.
 
 =====================================================================================*/

#include "qcbor.h"
#include "ieee754.h"


/*...... This is a ruler that is 80 characters long...........................*/


/*
 CBOR's two nesting types, arrays and maps, are tracked here. There is a
 limit of QCBOR_MAX_ARRAY_NESTING to the number of arrays and maps
 that can be nested in one encoding so the encoding context stays
 small enough to fit on the stack.
 
 When an array / map is opened, pCurrentNesting points to the element
 in pArrays that records the type, start position and accumluates a
 count of the number of items added. When closed the start position is
 used to go back and fill in the type and number of items in the array
 / map.
 
 Encoded output be just items like ints and strings that are
 not part of any array / map. That is, the first thing encoded
 does not have to be an array or a map.
 */
inline static void Nesting_Init(QCBORTrackNesting *pNesting)
{
   // assumes pNesting has been zeroed
   pNesting->pCurrentNesting = &pNesting->pArrays[0];
   // Implied CBOR array at the top nesting level. This is never returned,
   // but makes the item count work correctly.
   pNesting->pCurrentNesting->uMajorType = CBOR_MAJOR_TYPE_ARRAY;
}

inline static QCBORError Nesting_Increase(QCBORTrackNesting *pNesting, uint8_t uMajorType, uint32_t uPos)
{
   QCBORError nReturn = QCBOR_SUCCESS;
   
   if(pNesting->pCurrentNesting == &pNesting->pArrays[QCBOR_MAX_ARRAY_NESTING]) {
      // trying to open one too many
      nReturn = QCBOR_ERR_ARRAY_NESTING_TOO_DEEP;
   } else {
      pNesting->pCurrentNesting++;
      pNesting->pCurrentNesting->uCount     = 0;
      pNesting->pCurrentNesting->uStart     = uPos;
      pNesting->pCurrentNesting->uMajorType = uMajorType;
   }
   return nReturn;
}

inline static void Nesting_Decrease(QCBORTrackNesting *pNesting)
{
   pNesting->pCurrentNesting--;
}

inline static QCBORError Nesting_Increment(QCBORTrackNesting *pNesting, uint16_t uAmount)
{
   if(uAmount >= QCBOR_MAX_ITEMS_IN_ARRAY - pNesting->pCurrentNesting->uCount) {
      return QCBOR_ERR_ARRAY_TOO_LONG;
   }
      
   pNesting->pCurrentNesting->uCount += uAmount;
   return QCBOR_SUCCESS;
}

inline static uint16_t Nesting_GetCount(QCBORTrackNesting *pNesting)
{
   // The nesting count recorded is always the actual number of individiual
   // data items in the array or map. For arrays CBOR uses the actual item
   // count. For maps, CBOR uses the number of pairs.  This function returns
   // the number needed for the CBOR encoding, so it divides the number of
   // items by two for maps to get the number of pairs.  This implementation
   // takes advantage of the map major type being one larger the array major
   // type, hence the subtraction returns either 1 or 2.
   return pNesting->pCurrentNesting->uCount / (pNesting->pCurrentNesting->uMajorType - CBOR_MAJOR_TYPE_ARRAY+1);
}

inline static uint32_t Nesting_GetStartPos(QCBORTrackNesting *pNesting)
{
   return pNesting->pCurrentNesting->uStart;
}

inline static uint8_t Nesting_GetMajorType(QCBORTrackNesting *pNesting)
{
   return pNesting->pCurrentNesting->uMajorType;
}

inline static int Nesting_IsInNest(QCBORTrackNesting *pNesting)
{
   return pNesting->pCurrentNesting == &pNesting->pArrays[0] ? 0 : 1;
}




/*
 Error tracking plan -- Errors are tracked internally and not returned
 until Finish is called. The CBOR errors are in me->uError.
 UsefulOutBuf also tracks whether the buffer is full or not in its
 context.  Once either of these errors is set they are never
 cleared. Only Init() resets them. Or said another way, they must
 never be cleared or we'll tell the caller all is good when it is not.
 
 Only one error code is reported by Finish() even if there are
 multiple errors. The last one set wins. The caller might have to fix
 one error to reveal the next one they have to fix.  This is OK.
 
 The buffer full error tracked by UsefulBuf is only pulled out of
 UsefulBuf in Finish() so it is the one that usually wins.  UsefulBuf
 will never go off the end of the buffer even if it is called again
 and again when full.
 
 It is really tempting to not check for overflow on the count in the
 number of items in an array. It would save a lot of code, it is
 extremely unlikely that any one will every put 65,000 items in an
 array, and the only bad thing that would happen is the CBOR would be
 bogus.  Once we prove that is the only consequence, then we can make
 the change.
 
 Since this does not parse any input, you could in theory remove all
 error checks in this code if you knew the caller called it
 correctly. Maybe someday CDDL or some such language will be able to
 generate the code to call this and the calling code would always be
 correct. This could also automatically size some of the data
 structures like array/map nesting resulting in some good memory
 savings.
 
 Errors returned here fall into three categories:
 
 Sizes
   QCBOR_ERR_BUFFER_TOO_LARGE -- A buffer passed in > UINT32_MAX
   QCBOR_ERR_BUFFER_TOO_SMALL -- output buffer too small
 
   QCBOR_ERR_ARRAY_NESTING_TOO_DEEP -- Too many opens without closes
   QCBOR_ERR_ARRAY_TOO_LONG -- Too many things added to an array/map
 
 Nesting constructed incorrectly
   QCBOR_ERR_TOO_MANY_CLOSES -- more close calls than opens
   QCBOR_ERR_CLOSE_MISMATCH -- Type of close does not match open
   QCBOR_ERR_ARRAY_OR_MAP_STILL_OPEN -- Finish called without enough closes
 
 Bad data
   QCBOR_ERR_BAD_SIMPLE -- Simple value integer not valid
 
 */




/*
 Public function for initialization. See header qcbor.h
 */
void QCBOREncode_Init(QCBOREncodeContext *me, UsefulBuf Storage)
{
   memset(me, 0, sizeof(QCBOREncodeContext));
   if(Storage.len > UINT32_MAX) {
      me->uError = QCBOR_ERR_BUFFER_TOO_LARGE;
   } else {
      UsefulOutBuf_Init(&(me->OutBuf), Storage);
      Nesting_Init(&(me->nesting));
   }
}




/* 
 All CBOR data items have a type and a number. The number is either
 the value of the item for integer types, the length of the content
 for string, byte, array and map types, a tag for major type 6, and
 has serveral uses for major type 7.
 
 This function encodes the type and the number. There are several
 encodings for the number depending on how large it is and how it is
 used.
 
 Every encoding of the type and number has at least one byte, the 
 "initial byte".
 
 The top three bits of the initial byte are the major type for the
 CBOR data item.  The eight major types defined by the standard are
 defined as CBOR_MAJOR_TYPE_xxxx in qcbor.h.
 
 The remaining five bits, known as "additional information", and
 possibly more bytes encode the number. If the number is less than 24,
 then it is encoded entirely in the five bits. This is neat because it
 allows you to encode an entire CBOR data item in 1 byte for many
 values and types (integers 0-23, true, false, and tags).
 
 If the number is larger than 24, then it is encoded in 1,2,4 or 8
 additional bytes, with the number of these bytes indicated by the
 values of the 5 bits 24, 25, 25 and 27.
 
 It is possible to encode a particular number in many ways with this
 representation.  This implementation always uses the smallest
 possible representation. This is also the suggestion made in the RFC
 for cannonical CBOR.
 
 This function inserts them into the output buffer at the specified
 position. AppendEncodedTypeAndNumber() appends to the end.
 
 This function takes care of converting to network byte order. 
 
 This function is also used to insert floats and doubles. Before this
 function is called the float or double must be copied into a
 uint64_t. That is how they are passed in. They are then converted to
 network byte order correctly. The uMinLen param makes sure that even
 if all the digits of a halft, float or double are 0 it is still correctly
 encoded in 2, 4 or 8 bytes.
 
 */
#ifdef FORMAL
static void InsertEncodedTypeAndNumber(QCBOREncodeContext *me, uint8_t uMajorType, size_t uMinLen, uint64_t uNumber, size_t uPos)
{
   // No need to worry about integer overflow here because a) uMajorType is
   // always generated internally, not by the caller, b) this is for CBOR
   // _generation_, not parsing c) a mistake will result in bad CBOR generation,
   // not a security vulnerability.
   uMajorType <<= 5;
   
   if(uNumber > 0xffffffff || uMinLen >= 8) {
      UsefulOutBuf_InsertByte(&(me->OutBuf), uMajorType + LEN_IS_EIGHT_BYTES, uPos);
      UsefulOutBuf_InsertUint64(&(me->OutBuf), (uint64_t)uNumber, uPos+1);
      
   } else if(uNumber > 0xffff || uMinLen >= 4) {
      UsefulOutBuf_InsertByte(&(me->OutBuf), uMajorType + LEN_IS_FOUR_BYTES, uPos);
      UsefulOutBuf_InsertUint32(&(me->OutBuf), (uint32_t)uNumber, uPos+1);
      
   } else if (uNumber > 0xff || uMinLen>= 2) {
      // Between 0 and 65535
      UsefulOutBuf_InsertByte(&(me->OutBuf), uMajorType + LEN_IS_TWO_BYTES, uPos);
      UsefulOutBuf_InsertUint16(&(me->OutBuf), (uint16_t)uNumber, uPos+1);
      
   } else if(uNumber >= 24) {
      // Between 0 and 255, but only between 24 and 255 is ever encoded here
      UsefulOutBuf_InsertByte(&(me->OutBuf), uMajorType + LEN_IS_ONE_BYTE, uPos);
      UsefulOutBuf_InsertByte(&(me->OutBuf), (uint8_t)uNumber, uPos+1);

   } else {
      // Between 0 and 23
      UsefulOutBuf_InsertByte(&(me->OutBuf), uMajorType + (uint8_t)uNumber, uPos);
   }
}
#else

#include <arpa/inet.h>


static void InsertEncodedTypeAndNumber(QCBOREncodeContext *me, uint8_t uMajorType, size_t uMinLen, uint64_t uNumber, size_t uPos)
{
   uint8_t bytes[9];
   size_t  bytesLen;
   // No need to worry about integer overflow here because a) uMajorType is
   // always generated internally, not by the caller, b) this is for CBOR
   // _generation_, not parsing c) a mistake will result in bad CBOR generation,
   // not a security vulnerability.
   uMajorType <<= 5;
   
   if(uNumber > 0xffffffff || uMinLen >= 8) {
      bytes[0] =  uMajorType + LEN_IS_EIGHT_BYTES;
      uNumber = htonll(uNumber);
      memcpy(&bytes[1], &uNumber, sizeof(uint64_t));
      bytesLen = 1 + sizeof(uint64_t);
      
   } else if(uNumber > 0xffff || uMinLen >= 4) {
      bytes[0] =  uMajorType + LEN_IS_FOUR_BYTES;
      uint32_t uNumber32 = htonl(uNumber);
      memcpy(&bytes[1], &uNumber32, sizeof(uint32_t));
      bytesLen = 1 + sizeof(uint32_t);

   } else if (uNumber > 0xff || uMinLen>= 2) {
      // Between 0 and 65535
      bytes[0] =  uMajorType + LEN_IS_TWO_BYTES;
      uint16_t uNumber16 = htons(uNumber);
      memcpy(&bytes[1], &uNumber16, sizeof(uint16_t));
      bytesLen = sizeof(uint16_t) + 1;
      
   } else if(uNumber >= 24) {
      // Between 0 and 255, but only between 24 and 255 is ever encoded here
      bytes[0] =  uMajorType + LEN_IS_ONE_BYTE;
      uint8_t uNumber8 = uNumber;
      memcpy(&bytes[1], &uNumber8, sizeof(uint8_t));
      bytesLen = sizeof(uint8_t) + 1;
      
   } else {
      // Between 0 and 23
      bytes[0] = uMajorType + (uint8_t)uNumber;
      bytesLen = 1;
   }
   UsefulOutBuf_InsertData(&(me->OutBuf), bytes, bytesLen, uPos);
}
#endif



/*
 Append the type and number info to the end of the buffer.
 
 See InsertEncodedTypeAndNumber() function above for details
*/
inline static void AppendEncodedTypeAndNumber(QCBOREncodeContext *me, uint8_t uMajorType, uint64_t uNumber)
{
   // An append is an insert at the end.
   InsertEncodedTypeAndNumber(me, uMajorType, 0, uNumber, UsefulOutBuf_GetEndPosition(&(me->OutBuf)));
}



/*
 Public functions for closing arrays and maps. See header qcbor.h
 */
void QCBOREncode_AddUInt64(QCBOREncodeContext *me, uint64_t uValue)
{
   if(me->uError == QCBOR_SUCCESS) {
      AppendEncodedTypeAndNumber(me, CBOR_MAJOR_TYPE_POSITIVE_INT, uValue);
      me->uError = Nesting_Increment(&(me->nesting), 1);
   }
}


/*
 Public functions for closing arrays and maps. See header qcbor.h
 */
void QCBOREncode_AddInt64(QCBOREncodeContext *me, int64_t nNum)
{
   if(me->uError == QCBOR_SUCCESS) {
      uint8_t      uMajorType;
      uint64_t     uValue;
      
      if(nNum < 0) {
         uValue = (uint64_t)(-nNum - 1); // This is the way negative ints work in CBOR. -1 encodes as 0x00 with major type negative int.
         uMajorType = CBOR_MAJOR_TYPE_NEGATIVE_INT;
      } else {
         uValue = (uint64_t)nNum;
         uMajorType = CBOR_MAJOR_TYPE_POSITIVE_INT;
      }
      
      AppendEncodedTypeAndNumber(me, uMajorType, uValue);
      me->uError = Nesting_Increment(&(me->nesting), 1);
   }
}


/*
 Semi-private function. It is exposed to user of the interface,
 but they will usually call one of the inline wrappers rather than this.
 
 See header qcbor.h
 
 Does the work of adding some bytes to the CBOR output. Works for a
 byte and text strings, which are the same in in CBOR though they have
 different major types.  This is also used to insert raw
 pre-encoded CBOR.
 */
void QCBOREncode_AddBuffer(QCBOREncodeContext *me, uint8_t uMajorType, UsefulBufC Bytes)
{
   if(Bytes.len >= UINT32_MAX) {
      // This implementation doesn't allow buffers larger than UINT32_MAX.
      // This is primarily because QCBORTrackNesting.pArrays[].uStart is
      // an uint32 rather than size_t to keep the stack usage down. Also
      // it is entirely impractical to create tokens bigger than 4GB in
      // contiguous RAM
      me->uError = QCBOR_ERR_BUFFER_TOO_LARGE;
      
   } else {
      if(!me->uError) {
         // If it is not Raw CBOR, add the type and the length
         if(uMajorType != CBOR_MAJOR_NONE_TYPE_RAW) {
            AppendEncodedTypeAndNumber(me, uMajorType, Bytes.len);
            // The increment in uPos is to account for bytes added for
            // type and number so the buffer being added goes to the
            // right place
         }
         
         // Actually add the bytes
         UsefulOutBuf_AppendUsefulBuf(&(me->OutBuf), Bytes);
         
         // Update the array counting if there is any nesting at all
         me->uError = Nesting_Increment(&(me->nesting), 1);
      }
   }
}


/*
 Public functions for closing arrays and maps. See header qcbor.h
 */
void QCBOREncode_AddTag(QCBOREncodeContext *me, uint64_t uTag)
{
   AppendEncodedTypeAndNumber(me, CBOR_MAJOR_TYPE_OPTIONAL, uTag);
}




/*
 Semi-private function. It is exposed to user of the interface,
 but they will usually call one of the inline wrappers rather than this.
 
 See header qcbor.h
 */
void QCBOREncode_AddType7(QCBOREncodeContext *me, size_t uSize, uint64_t uNum)
{
   if(me->uError == QCBOR_SUCCESS) {
      // This function call takes care of endian swapping for the float / double
      InsertEncodedTypeAndNumber(me,
                                 CBOR_MAJOR_TYPE_SIMPLE,  // The major type for
                                 // floats and doubles
                                 uSize,                   // min size / tells
                                 // encoder to do it right
                                 uNum,                    // Bytes of the floating
                                 // point number as a uint
                                 UsefulOutBuf_GetEndPosition(&(me->OutBuf))); // end position for append
      
      me->uError = Nesting_Increment(&(me->nesting), 1);
   }
}


/*
 Public functions for closing arrays and maps. See header qcbor.h
 */
void QCBOREncode_AddDouble(QCBOREncodeContext *me, double dNum)
{
   const IEEE754_union uNum = IEEE754_DoubleToSmallest(dNum);
   
   QCBOREncode_AddType7(me, uNum.uSize, uNum.uValue);
}


/*
 Semi-public function. It is exposed to user of the interface,
 but they will usually call one of the inline wrappers rather than this.
 
 See header qcbor.h
*/
void QCBOREncode_OpenMapOrArray(QCBOREncodeContext *me, uint8_t uMajorType)
{
      // Add one item to the nesting level we are in for the new map or array
      me->uError = Nesting_Increment(&(me->nesting), 1);
      if(!me->uError) {
         // Increase nesting level because this is a map or array
         // Cast from size_t to uin32_t is safe because the UsefulOutBuf
         // size is limited to UINT32_MAX in QCBOR_Init().
         me->uError = Nesting_Increase(&(me->nesting), uMajorType, (uint32_t)UsefulOutBuf_GetEndPosition(&(me->OutBuf)));
      }
}


/*
 Public functions for closing arrays and maps. See header qcbor.h
 */
void QCBOREncode_CloseMapOrArray(QCBOREncodeContext *me, uint8_t uMajorType, UsefulBufC *pWrappedCBOR)
{
   if(!me->uError) {
      if(!Nesting_IsInNest(&(me->nesting))) {
         me->uError = QCBOR_ERR_TOO_MANY_CLOSES;
      } else if( Nesting_GetMajorType(&(me->nesting)) != uMajorType) {
         me->uError = QCBOR_ERR_CLOSE_MISMATCH;
      } else {
         // When the array, map or bstr wrap was started, nothing was done
         // except note the position of the start of it. This code goes back
         // and inserts the actual CBOR array, map or bstr and its length.
         // That means all the data that is in the array, map or wrapped
         // needs to be slid to the right. This is done by UsefulOutBuf's
         // insert function that is called from inside
         // InsertEncodedTypeAndNumber()
         const size_t uInsertPosition         = Nesting_GetStartPos(&(me->nesting));
         const size_t uEndPosition            = UsefulOutBuf_GetEndPosition(&(me->OutBuf));
         // This can't go negative because the UsefulOutBuf always only grows
         // and never shrinks. UsefulOutBut itself also has defenses such that
         // it won't write were it should not even if given hostile input lengths
         const size_t uLenOfEncodedMapOrArray = uEndPosition - uInsertPosition;
         
         // Length is number of bytes for a bstr and number of items a for map & array
         const size_t uLength = uMajorType == CBOR_MAJOR_TYPE_BYTE_STRING ?
                                    uLenOfEncodedMapOrArray : Nesting_GetCount(&(me->nesting));
         
         // Actually insert
         InsertEncodedTypeAndNumber(me,
                                    uMajorType,       // major type bstr, array or map
                                    0,                // no minimum length for encoding
                                    uLength,          // either len of bstr or num items in array or map
                                    uInsertPosition); // position in out buffer
         
         // Return pointer and length to the enclosed encoded CBOR. The intended
         // use is for it to be hashed (e.g., SHA-256) in a COSE implementation.
         // This must be used right away, as the pointer and length go invalid
         // on any subsequent calls to this function because of the
         // InsertEncodedTypeAndNumber() call that slides data to the right.
         if(pWrappedCBOR) {
            UsefulBufC PartialResult = UsefulOutBuf_OutUBuf(&(me->OutBuf));
            size_t uBstrLen = UsefulOutBuf_GetEndPosition(&(me->OutBuf)) - uEndPosition;
            *pWrappedCBOR = UsefulBuf_Tail(PartialResult, uInsertPosition+uBstrLen);
         }
         Nesting_Decrease(&(me->nesting));
      }
   }
}



/*
 Public functions to finish and get the encoded result. See header qcbor.h
 */
QCBORError QCBOREncode_Finish(QCBOREncodeContext *me, UsefulBufC *pEncodedCBOR)
{
   QCBORError uReturn = me->uError;
   
   if(uReturn != QCBOR_SUCCESS) {
      goto Done;
   }
   
   if (Nesting_IsInNest(&(me->nesting))) {
      uReturn = QCBOR_ERR_ARRAY_OR_MAP_STILL_OPEN;
      goto Done;
   }
   
   if(UsefulOutBuf_GetError(&(me->OutBuf))) {
      // Stuff didn't fit in the buffer.
      // This check catches this condition for all the appends and inserts
      // so checks aren't needed when the appends and inserts are performed.
      // And of course UsefulBuf will never overrun the input buffer given
      // to it. No complex analysis of the error handling in this file is
      // needed to know that is true. Just read the UsefulBuf code.
      uReturn = QCBOR_ERR_BUFFER_TOO_SMALL;
      goto Done;
   }

   *pEncodedCBOR = UsefulOutBuf_OutUBuf(&(me->OutBuf));
   
Done:
   return uReturn;
}


/*
 Public functions to finish and get the encoded result. See header qcbor.h
 */
QCBORError QCBOREncode_FinishGetSize(QCBOREncodeContext *me, size_t *puEncodedLen)
{
   UsefulBufC Enc;
   
   QCBORError nReturn = QCBOREncode_Finish(me, &Enc);
   
   if(nReturn == QCBOR_SUCCESS) {
      *puEncodedLen = Enc.len;
   }
   
   return nReturn;
}




/*
 Notes on the code
 
 CBOR Major Type     Public Function
 0                   QCBOREncode_AddUInt64
 0, 1                QCBOREncode_AddUInt64, QCBOREncode_AddInt64
 2, 3                QCBOREncode_AddBuffer, Also QCBOREncode_OpenMapOrArray
 4, 5                QCBOREncode_OpenMapOrArray
 6                   QCBOREncode_AddTag
 7                   QCBOREncode_AddDouble, QCBOREncode_AddSimple
 
 Object code sizes on X86 with LLVM compiler and -Os (Nov 27, 2018)
 
 _QCBOREncode_Init   84
 _QCBOREncode_AddUInt64   76
 _QCBOREncode_AddInt64   87
 _QCBOREncode_AddBuffer   131
 _QCBOREncode_AddSimple   30
 _AppendType7   83
 _QCBOREncode_OpenMapOrArray   89
 _QCBOREncode_CloseMapOrArray   181
 _InsertEncodedTypeAndNumber   480
 _QCBOREncode_Finish   72
 
 Total is about 1.4KB (including FinishGetSize and AddTag and AddDouble)
 
 _InsertEncodedTypeAndNumber is large because a lot of UsefulBuf
 code inlines into it including the conversion to network byte
 order. This could be optimized to at least half the size, but
 code would probably not be quite as clean.
 
 _QCBOREncode_CloseMapOrArray is larger because it has a lot
 of nesting tracking to do and much of Nesting_ inlines
 into it. It probably can't be reduced much.
 
 If the error returned by Nesting_Increment() can be ignored
 because the limit is so high and the consequence of exceeding
 is proved to be inconsequential, then a lot of if(me->uError)
 instance can be removed, saving some code.
 
 */


/*
 _InsertEncodedTypeAndNumber:            ## @InsertEncodedTypeAndNumber
Lfunc_begin10:
	.loc	7 307 0                 ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:307:0
	.cfi_startproc
## BB#0:
	pushq	%rbp
Lcfi30:
	.cfi_def_cfa_offset 16
Lcfi31:
	.cfi_offset %rbp, -16
	movq	%rsp, %rbp
Lcfi32:
	subq	$80, %rsp
	movb	%sil, %al
	movl	$4294967295, %esi       ## imm = 0xFFFFFFFF
	movl	%esi, %r9d
	movq	(%r10), %r10
	movq	%r10, -8(%rbp)
	movq	%rdi, -32(%rbp)
	movb	%al, -33(%rbp)
	movq	%rdx, -48(%rbp)
	movq	%rcx, -56(%rbp)
	movq	%r8, -64(%rbp)
Ltmp50:
	.loc	7 314 15 prologue_end   ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:314:15
	movzbl	-33(%rbp), %esi
	shll	$5, %esi
	movb	%sil, %al
	movb	%al, -33(%rbp)
Ltmp51:
	.loc	7 316 15                ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:316:15
	cmpq	%r9, -56(%rbp)
	.loc	7 316 28 is_stmt 0      ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:316:28
	ja	LBB10_2
## BB#1:
	.loc	7 316 39                ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:316:39
	cmpq	$8, -48(%rbp)
Ltmp52:
	.loc	7 316 7                 ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:316:7
	jb	LBB10_3
LBB10_2:
Ltmp53:
	.loc	7 317 19 is_stmt 1      ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:317:19
	movzbl	-33(%rbp), %eax
	.loc	7 317 30 is_stmt 0      ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:317:30
	addl	$27, %eax
	.loc	7 317 19                ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:317:19
	movb	%al, %cl
	.loc	7 317 16                ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:317:16
	movb	%cl, -17(%rbp)
	.loc	7 318 17 is_stmt 1      ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:318:17
	movq	-56(%rbp), %rdi
	callq	__OSSwapInt64
	.loc	7 318 15 is_stmt 0      ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:318:15
	movq	%rax, -56(%rbp)
	.loc	7 319 7 is_stmt 1       ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:319:7
	movq	-56(%rbp), %rax
	movq	%rax, -16(%rbp)
	.loc	7 320 16                ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:320:16
	movq	$9, -72(%rbp)
	.loc	7 322 4                 ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:322:4
	jmp	LBB10_15
Ltmp54:
LBB10_3:
	.loc	7 322 22 is_stmt 0      ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:322:22
	cmpq	$65535, -56(%rbp)       ## imm = 0xFFFF
	.loc	7 322 31                ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:322:31
	ja	LBB10_5
## BB#4:
	.loc	7 322 42                ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:322:42
	cmpq	$4, -48(%rbp)
Ltmp55:
	.loc	7 322 14                ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:322:14
	jb	LBB10_6
LBB10_5:
Ltmp56:
	.loc	7 323 19 is_stmt 1      ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:323:19
	movzbl	-33(%rbp), %eax
	.loc	7 323 30 is_stmt 0      ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:323:30
	addl	$26, %eax
	.loc	7 323 19                ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:323:19
	movb	%al, %cl
	.loc	7 323 16                ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:323:16
	movb	%cl, -17(%rbp)
	.loc	7 324 28 is_stmt 1      ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:324:28
	movq	-56(%rbp), %rdx
	movl	%edx, %eax
	movl	%eax, %edi
	callq	__OSSwapInt32
	.loc	7 324 16 is_stmt 0      ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:324:16
	movl	%eax, -76(%rbp)
	.loc	7 325 7 is_stmt 1       ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:325:7
	movl	-76(%rbp), %eax
	movl	%eax, -16(%rbp)
	.loc	7 326 16                ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:326:16
	movq	$5, -72(%rbp)
	.loc	7 328 4                 ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:328:4
	jmp	LBB10_14
Ltmp57:
LBB10_6:
	.loc	7 328 23 is_stmt 0      ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:328:23
	cmpq	$255, -56(%rbp)
	.loc	7 328 30                ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:328:30
	ja	LBB10_8
## BB#7:
	.loc	7 328 40                ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:328:40
	cmpq	$2, -48(%rbp)
Ltmp58:
	.loc	7 328 15                ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:328:15
	jb	LBB10_9
LBB10_8:
Ltmp59:
	.loc	7 330 19 is_stmt 1      ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:330:19
	movzbl	-33(%rbp), %eax
	.loc	7 330 30 is_stmt 0      ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:330:30
	addl	$25, %eax
	.loc	7 330 19                ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:330:19
	movb	%al, %cl
	.loc	7 330 16                ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:330:16
	movb	%cl, -17(%rbp)
	.loc	7 331 28 is_stmt 1      ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:331:28
	movq	-56(%rbp), %rdx
	movw	%dx, %si
	movzwl	%si, %edi
	callq	__OSSwapInt16
	movzwl	%ax, %edi
	movw	%di, %ax
	.loc	7 331 16 is_stmt 0      ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:331:16
	movw	%ax, -78(%rbp)
	.loc	7 332 7 is_stmt 1       ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:332:7
	movw	-78(%rbp), %ax
	movw	%ax, -16(%rbp)
	.loc	7 333 16                ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:333:16
	movq	$3, -72(%rbp)
	.loc	7 335 4                 ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:335:4
	jmp	LBB10_13
Ltmp60:
LBB10_9:
	.loc	7 335 22 is_stmt 0      ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:335:22
	cmpq	$24, -56(%rbp)
Ltmp61:
	.loc	7 335 14                ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:335:14
	jb	LBB10_11
## BB#10:
Ltmp62:
	.loc	7 337 19 is_stmt 1      ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:337:19
	movzbl	-33(%rbp), %eax
	.loc	7 337 30 is_stmt 0      ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:337:30
	addl	$24, %eax
	.loc	7 337 19                ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:337:19
	movb	%al, %cl
	.loc	7 337 16                ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:337:16
	movb	%cl, -17(%rbp)
	.loc	7 338 26 is_stmt 1      ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:338:26
	movq	-56(%rbp), %rdx
	movb	%dl, %cl
	.loc	7 338 15 is_stmt 0      ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:338:15
	movb	%cl, -79(%rbp)
	.loc	7 339 7 is_stmt 1       ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:339:7
	movb	-79(%rbp), %cl
	movb	%cl, -16(%rbp)
	.loc	7 340 16                ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:340:16
	movq	$2, -72(%rbp)
	.loc	7 342 4                 ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:342:4
	jmp	LBB10_12
Ltmp63:
LBB10_11:
	.loc	7 344 18                ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:344:18
	movzbl	-33(%rbp), %eax
	.loc	7 344 40 is_stmt 0      ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:344:40
	movq	-56(%rbp), %rcx
	.loc	7 344 31                ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:344:31
	movb	%cl, %dl
	movzbl	%dl, %esi
	.loc	7 344 29                ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:344:29
	addl	%esi, %eax
	.loc	7 344 18                ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:344:18
	movb	%al, %dl
	.loc	7 344 16                ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:344:16
	movb	%dl, -17(%rbp)
	.loc	7 345 16 is_stmt 1      ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:345:16
	movq	$1, -72(%rbp)
Ltmp64:
LBB10_12:
	.loc	7 0 16 is_stmt 0        ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:0:16
	jmp	LBB10_13
LBB10_13:
	jmp	LBB10_14
LBB10_14:
	jmp	LBB10_15
LBB10_15:
	leaq	-17(%rbp), %rsi
	.loc	7 347 30 is_stmt 1      ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:347:30
	movq	-32(%rbp), %rdi
	.loc	7 347 50 is_stmt 0      ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:347:50
	movq	-72(%rbp), %rdx
	.loc	7 347 60                ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:347:60
	movq	-64(%rbp), %rcx
	.loc	7 347 4                 ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:347:4
	callq	_UsefulOutBuf_InsertData
	movq	___stack_chk_guard@GOTPCREL(%rip), %rcx
	movq	(%rcx), %rcx
	movq	-8(%rbp), %rdx
	cmpq	%rdx, %rcx
	jne	LBB10_17
## BB#16:
	.loc	7 348 1 is_stmt 1       ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:348:1
	addq	$80, %rsp
	popq	%rbp
	retq
LBB10_17:
	.loc	7 0 0 is_stmt 0         ## /Users/laurencelundblade/Code/QCBOR/master/src/qcbor_encode.c:0:0
	callq	___stack_chk_fail
Ltmp65:
Lfunc_end10:
	.cfi_endproc






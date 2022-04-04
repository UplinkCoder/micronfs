#ifndef __ENDIAN_H_
#define __ENDIAN_H_

#ifdef _WIN32
#  include "stdint_msvc.h"
#else
#  include <stdint.h>
#endif

#define NTOHL HTONL
#define NTOHS HTONS

#if !defined(ENDIAN_IS_LITTLE) && !defined(ENDIAN_IS_BIG)
#   if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && defined(__ORDER_BIG_ENDIAN__)
#       if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#            define ENDIAN_IS_LITTLE
#        elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#            define ENDIAN_IS_BIG
#        else
#            error "endian.h support little or big endian only."
#        endif
#   elif defined(_WIN32)
#        define ENDIAN_IS_LITTLE
#   else
#       include <endian.h>
#       if __BYTE_ORDER == __LITTLE_ENDIAN
#           define ENDIAN_IS_LITTLE
#       elif __BYTE_ORDER == __BIG_ENDIAN
#           define ENDIAN_IS_BIG
#       endif
#   endif
#endif

#ifdef ENDIAN_IS_LITTLE

#define HTONS(VAL) \
    ((((VAL) & 0xff) << 8) \
  | (((VAL) >> 8) & 0xff))

#define HTONL(VAL) \
    ((((VAL) & 0xff) << 24)  \
  |  (((VAL) >> 24) & 0xff)  \
  |  (((VAL) & 0xff00) << 8) \
  |  (((VAL) >> 8) & 0xff00))

#elif ENDIAN_IS_BIG

#define HTONS(VAL) \
    (VAL)

#define HTONL(VAL) \
    (VAL)

#else
#  error("Cannot determine endianness");
#endif

#endif

#include "libucd_int.h"

const struct _libucd_property_array _libucd_property_array[] =
  {
#include "gen/proparray.c"
    { .ucs = UCS_CNT }		/* Sentinel at end of list */
  };

const int _libucd_property_array_count =
  (sizeof _libucd_property_array)/sizeof(struct _libucd_property_array)-1;


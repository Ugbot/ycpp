// ycpp.h — umbrella header. Single `#include <ycpp/ycpp.h>` pulls the W1 surface.
//
// Later waves extend this file as new modules land (struct store, doc,
// types, encoders, undo, gc).

#pragma once

#include "ycpp_arena.h"
#include "ycpp_awareness.h"
#include "ycpp_byteview.h"
#include "ycpp_delete_set.h"
#include "ycpp_doc.h"
#include "ycpp_envelope.h"
#include "ycpp_hashmap.h"
#include "ycpp_id.h"
#include "ycpp_item.h"
#include "ycpp_pool.h"
#include "ycpp_protocol.h"
#include "ycpp_reader.h"
#include "ycpp_ring.h"
#include "ycpp_state_vector.h"
#include "ycpp_status.h"
#include "ycpp_struct_store.h"
#include "ycpp_update.h"
#include "ycpp_varint.h"
#include "ycpp_writer.h"
#include "ycpp_yarray.h"
#include "ycpp_ymap.h"
#include "ycpp_ytext.h"

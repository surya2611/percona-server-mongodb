/* This file was generated by upbc (the upb compiler) from the input
 * file:
 *
 *     xds/service/orca/v3/orca.proto
 *
 * Do not edit -- your changes will be discarded when the file is
 * regenerated. */

#include <stddef.h>
#include "upb/msg_internal.h"
#include "xds/service/orca/v3/orca.upb.h"
#include "xds/data/orca/v3/orca_load_report.upb.h"
#include "google/protobuf/duration.upb.h"
#include "validate/validate.upb.h"

#include "upb/port_def.inc"

static const upb_MiniTable_Sub xds_service_orca_v3_OrcaLoadReportRequest_submsgs[1] = {
  {.submsg = &google_protobuf_Duration_msginit},
};

static const upb_MiniTable_Field xds_service_orca_v3_OrcaLoadReportRequest__fields[2] = {
  {1, UPB_SIZE(4, 8), UPB_SIZE(1, 1), 0, 11, kUpb_FieldMode_Scalar | (kUpb_FieldRep_Pointer << kUpb_FieldRep_Shift)},
  {2, UPB_SIZE(8, 16), UPB_SIZE(0, 0), kUpb_NoSub, 9, kUpb_FieldMode_Array | (kUpb_FieldRep_Pointer << kUpb_FieldRep_Shift)},
};

const upb_MiniTable xds_service_orca_v3_OrcaLoadReportRequest_msginit = {
  &xds_service_orca_v3_OrcaLoadReportRequest_submsgs[0],
  &xds_service_orca_v3_OrcaLoadReportRequest__fields[0],
  UPB_SIZE(12, 24), 2, kUpb_ExtMode_NonExtendable, 2, 255, 0,
};

static const upb_MiniTable *messages_layout[1] = {
  &xds_service_orca_v3_OrcaLoadReportRequest_msginit,
};

const upb_MiniTable_File xds_service_orca_v3_orca_proto_upb_file_layout = {
  messages_layout,
  NULL,
  NULL,
  1,
  0,
  0,
};

#include "upb/port_undef.inc"


set(ENIP_PROTOCOL_SOURCES
                     "${CMAKE_CURRENT_SOURCE_DIR}/common/plc_type.h"
                     "${CMAKE_CURRENT_SOURCE_DIR}/common/plc_classify.h"
                     "${CMAKE_CURRENT_SOURCE_DIR}/common/plc_classify.c"
                     "${CMAKE_CURRENT_SOURCE_DIR}/client/enip_type.h"
                     "${CMAKE_CURRENT_SOURCE_DIR}/client/enip_type.c"
                     "${CMAKE_CURRENT_SOURCE_DIR}/client/enip_cip.h"
                     "${CMAKE_CURRENT_SOURCE_DIR}/client/enip_cip.c"
                     "${CMAKE_CURRENT_SOURCE_DIR}/client/enip_eip.h"
                     "${CMAKE_CURRENT_SOURCE_DIR}/client/enip_eip.c"
                     "${CMAKE_CURRENT_SOURCE_DIR}/client/enip_cpf.h"
                     "${CMAKE_CURRENT_SOURCE_DIR}/client/enip_cpf.c"
                     "${CMAKE_CURRENT_SOURCE_DIR}/client/enip_dialect.h"
                     "${CMAKE_CURRENT_SOURCE_DIR}/client/enip_session.h"
                     "${CMAKE_CURRENT_SOURCE_DIR}/client/enip_session.c"
                     "${CMAKE_CURRENT_SOURCE_DIR}/client/enip_tag.h"
                     "${CMAKE_CURRENT_SOURCE_DIR}/client/enip_tag.c"
                     "${CMAKE_CURRENT_SOURCE_DIR}/client/enip_pccc_addr.h"
                     "${CMAKE_CURRENT_SOURCE_DIR}/client/enip_pccc_addr.c"
                     "${CMAKE_CURRENT_SOURCE_DIR}/client/enip_discover.h"
                     "${CMAKE_CURRENT_SOURCE_DIR}/client/enip_discover.c"
                     "${CMAKE_CURRENT_SOURCE_DIR}/client/enip.h"
                     "${CMAKE_CURRENT_SOURCE_DIR}/client/enip.c"
                    PARENT_SCOPE
)

# Server-tag sources (device simulator, folded in from the former libdevsim;
# see SERVER_TAGS.md).  common/ is the direction-agnostic EIP/CPF/CIP/identity
# codec — server decode today, client encode adopts it in Phase B.  server/ is
# the listener + endpoint runtime.  dialects/ is vendor-specific listing +
# PCCC data path, plugged in via the CIP object registry.
set(ENIP_SERVER_SOURCES
                     "${CMAKE_CURRENT_SOURCE_DIR}/common/cip.h"
                     "${CMAKE_CURRENT_SOURCE_DIR}/common/cip.c"
                     "${CMAKE_CURRENT_SOURCE_DIR}/common/cpf.h"
                     "${CMAKE_CURRENT_SOURCE_DIR}/common/cpf.c"
                     "${CMAKE_CURRENT_SOURCE_DIR}/common/eip.h"
                     "${CMAKE_CURRENT_SOURCE_DIR}/common/eip.c"
                     "${CMAKE_CURRENT_SOURCE_DIR}/common/identity.h"
                     "${CMAKE_CURRENT_SOURCE_DIR}/common/identity.c"
                     "${CMAKE_CURRENT_SOURCE_DIR}/server/device.h"
                     "${CMAKE_CURRENT_SOURCE_DIR}/server/device_sim.h"
                     "${CMAKE_CURRENT_SOURCE_DIR}/server/device_sim.c"
                     "${CMAKE_CURRENT_SOURCE_DIR}/server/server.h"
                     "${CMAKE_CURRENT_SOURCE_DIR}/server/server.c"
                     "${CMAKE_CURRENT_SOURCE_DIR}/server/discovery.h"
                     "${CMAKE_CURRENT_SOURCE_DIR}/server/discovery.c"
                     "${CMAKE_CURRENT_SOURCE_DIR}/server/endpoint.h"
                     "${CMAKE_CURRENT_SOURCE_DIR}/server/endpoint.c"
                     "${CMAKE_CURRENT_SOURCE_DIR}/server/eip_server_tag.h"
                     "${CMAKE_CURRENT_SOURCE_DIR}/server/eip_server_tag.c"
                     "${CMAKE_CURRENT_SOURCE_DIR}/dialects/rockwell/ab_listing.h"
                     "${CMAKE_CURRENT_SOURCE_DIR}/dialects/rockwell/ab_listing.c"
                     "${CMAKE_CURRENT_SOURCE_DIR}/dialects/pccc/pccc.h"
                     "${CMAKE_CURRENT_SOURCE_DIR}/dialects/pccc/pccc.c"
                    PARENT_SCOPE
)

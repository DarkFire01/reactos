/*
 * PROJECT:     ReactOS SDK
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Windows AppModel identity length limits
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#define PACKAGE_ARCHITECTURE_MIN_LENGTH     3
#define PACKAGE_ARCHITECTURE_MAX_LENGTH     7
#define PACKAGE_VERSION_MIN_LENGTH          7
#define PACKAGE_VERSION_MAX_LENGTH          23
#define PACKAGE_NAME_MIN_LENGTH             3
#define PACKAGE_NAME_MAX_LENGTH             50
#define PACKAGE_PUBLISHER_MIN_LENGTH        3
#define PACKAGE_PUBLISHER_MAX_LENGTH        8192
#define PACKAGE_PUBLISHERID_MIN_LENGTH      13
#define PACKAGE_PUBLISHERID_MAX_LENGTH      13
#define PACKAGE_RESOURCEID_MIN_LENGTH       0
#define PACKAGE_RESOURCEID_MAX_LENGTH       30

#define PACKAGE_FULL_NAME_MIN_LENGTH        (PACKAGE_NAME_MIN_LENGTH + 1 + \
                                             PACKAGE_VERSION_MIN_LENGTH + 1 + \
                                             PACKAGE_ARCHITECTURE_MIN_LENGTH + 1 + \
                                             PACKAGE_RESOURCEID_MIN_LENGTH + 1 + \
                                             PACKAGE_PUBLISHERID_MIN_LENGTH)
#define PACKAGE_FULL_NAME_MAX_LENGTH        (PACKAGE_NAME_MAX_LENGTH + 1 + \
                                             PACKAGE_VERSION_MAX_LENGTH + 1 + \
                                             PACKAGE_ARCHITECTURE_MAX_LENGTH + 1 + \
                                             PACKAGE_RESOURCEID_MAX_LENGTH + 1 + \
                                             PACKAGE_PUBLISHERID_MAX_LENGTH)

#define PACKAGE_FAMILY_NAME_MIN_LENGTH      (PACKAGE_NAME_MIN_LENGTH + 1 + PACKAGE_PUBLISHERID_MIN_LENGTH)
#define PACKAGE_FAMILY_NAME_MAX_LENGTH      (PACKAGE_NAME_MAX_LENGTH + 1 + PACKAGE_PUBLISHERID_MAX_LENGTH)

#define PACKAGE_MIN_DEPENDENCIES                0
#define PACKAGE_MAX_DEPENDENCIES                128
#define PACKAGE_FAMILY_MIN_RESOURCE_PACKAGES    0
#define PACKAGE_FAMILY_MAX_RESOURCE_PACKAGES    512
#define PACKAGE_GRAPH_MIN_SIZE                  1
#define PACKAGE_GRAPH_MAX_SIZE                  (1 + PACKAGE_MAX_DEPENDENCIES + PACKAGE_FAMILY_MAX_RESOURCE_PACKAGES)

#define PACKAGE_APPLICATIONS_MIN_COUNT              0
#define PACKAGE_APPLICATIONS_MAX_COUNT              100
#define PACKAGE_RELATIVE_APPLICATION_ID_MIN_LENGTH  (1 + 1)
#define PACKAGE_RELATIVE_APPLICATION_ID_MAX_LENGTH  (64 + 1)

#define APPLICATION_USER_MODEL_ID_MIN_LENGTH        (PACKAGE_FAMILY_NAME_MIN_LENGTH + 1 + \
                                                     PACKAGE_RELATIVE_APPLICATION_ID_MIN_LENGTH)
#define APPLICATION_USER_MODEL_ID_MAX_LENGTH        (PACKAGE_FAMILY_NAME_MAX_LENGTH + 1 + \
                                                     PACKAGE_RELATIVE_APPLICATION_ID_MAX_LENGTH)

##########################################################################
# If not stated otherwise in this file or this component's LICENSE
# file the following copyright and licenses apply:
#
# Copyright 2019 RDK Management
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
##########################################################################
include_HEADERS = xrsr.h xrsr_config.json
lib_LTLIBRARIES = libxrsr.la

libxrsr_la_SOURCES = xrsr_version.h       \
                     xrsr.c               \
                     xrsr_msgq.c          \
                     xrsr_xraudio.c       \
                     xrsr_utils.c         

libxrsr_la_CFLAGS  = 
libxrsr_la_LDFLAGS = 

if HTTP_ENABLED
libxrsr_la_SOURCES += xrsr_protocol_http.c
libxrsr_la_CFLAGS  += -DHTTP_ENABLED
libxrsr_la_LDFLAGS += -lcurl
endif

if WS_ENABLED
libxrsr_la_SOURCES += xrsr_protocol_ws.c
libxrsr_la_CFLAGS  += -DWS_ENABLED
libxrsr_la_LDFLAGS += -lnopoll
endif

if SDT_ENABLED
libxrsr_la_SOURCES += xrsr_protocol_sdt.c
libxrsr_la_CFLAGS  += -DSDT_ENABLED
endif

BUILT_SOURCES = xrsr_version.h xrsr_config.h xrsr_config.json
CLEANFILES    = xrsr_version.h xrsr_config.h xrsr_config.json

xrsr_version.h:
	echo "#define XRSR_VER_MAJOR \"1\"" >$@
	echo "#define XRSR_VER_MINOR \"`git -C ${srcdir} rev-list --count 4b825dc642cb6eb9a060e54bf8d69288fbee4904..HEAD``git -C ${srcdir} diff --quiet || echo \"++\"`\"" >>$@
	echo "#define XRSR_BRANCH    \"${GIT_BRANCH}\"" >>$@
	echo "#define XRSR_COMMIT_ID \"`git -C ${srcdir} log --format="%H" -n 1`\"" >>$@
	echo "#define XRSR_VERSION (XRSR_VER_MAJOR \".\" XRSR_VER_MINOR)" >>$@

xrsr_config.json: xrsr_config_default.json
	python3 "${VSDK_UTILS_JSON_COMBINE}" -i $< -a "${XRSR_CONFIG_JSON_XRAUDIO}:xraudio" -s "${XRSR_CONFIG_JSON_SUB}" -a "${XRSR_CONFIG_JSON_ADD}" -o $@

xrsr_config.h: xrsr_config.json
	python3 "${VSDK_UTILS_JSON_TO_HEADER}" -i $< -o $@ -v "ws,http"

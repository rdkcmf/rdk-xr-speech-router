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
AC_INIT([xr-speech-router], [1.0], [David_Wolaver@cable.comcast.com])
AM_INIT_AUTOMAKE([foreign])
AM_PROG_AR
LT_INIT

AC_PROG_CC

AC_CONFIG_MACRO_DIRS([m4])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_FILES([
 Makefile
 src/Makefile
])

AC_ARG_ENABLE([rdkxlogger],
[  --enable-rdkxlogger    Turn on RDKX logger support],
[case "${enableval}" in
  yes) rdkxlogger=true ;;
  no)  rdkxlogger=false ;;
  *) AC_MSG_ERROR([bad value ${enableval} for --enable-rdkxlogger]) ;;
esac],[rdkxlogger=false])
AM_CONDITIONAL([RDKX_LOGGER_ENABLED], [test x$rdkxlogger = xtrue])


AC_ARG_ENABLE([xrsr_http],
[  --enable-xrsr_http    Turn on HTTP support],
[case "${enableval}" in
  yes) xrsr_http=true ;;
  no)  xrsr_http=false ;;
  *) AC_MSG_ERROR([bad value ${enableval} for --enable-xrsr_http]) ;;
esac],[xrsr_http=false])
AM_CONDITIONAL([HTTP_ENABLED], [test x$xrsr_http = xtrue])

AC_ARG_ENABLE([xrsr_ws],
[  --enable-xrsr_ws    Turn on WS support],
[case "${enableval}" in
  yes) xrsr_ws=true ;;
  no)  xrsr_ws=false ;;
  *) AC_MSG_ERROR([bad value ${enableval} for --enable-xrsr_ws]) ;;
esac],[xrsr_ws=false])
AM_CONDITIONAL([WS_ENABLED], [test x$xrsr_ws = xtrue])

AC_ARG_VAR(VSDK_UTILS_JSON_TO_HEADER, script to create header from json object)
AC_ARG_VAR(VSDK_UTILS_JSON_COMBINE,   script to combine multiple json files)

AC_ARG_VAR(XRSR_CONFIG_JSON_XRAUDIO, xraudio json configuration file)
AC_ARG_VAR(XRSR_CONFIG_JSON_SUB, oem sub json configuration file)
AC_ARG_VAR(XRSR_CONFIG_JSON_ADD, oem add json configuration file)
AC_ARG_VAR(GIT_BRANCH, git branch name)

AC_OUTPUT

/*
    SPDX-FileCopyrightText: 2014-2017 Milian Wolff <mail@milianw.de>

    SPDX-License-Identifier: LGPL-2.1-or-later
*/

#ifndef HEAPTRACK_CONFIG_H
#define HEAPTRACK_CONFIG_H

#define HEAPTRACK_VERSION_STRING "@HEAPTRACK_VERSION_MAJOR@.@HEAPTRACK_VERSION_MINOR@.@HEAPTRACK_VERSION_PATCH@"
#define HEAPTRACK_VERSION_MAJOR @HEAPTRACK_VERSION_MAJOR@
#define HEAPTRACK_VERSION_MINOR @HEAPTRACK_VERSION_MINOR@
#define HEAPTRACK_VERSION_PATCH @HEAPTRACK_VERSION_PATCH@
#define HEAPTRACK_VERSION ((HEAPTRACK_VERSION_MAJOR<<16)|(HEAPTRACK_VERSION_MINOR<<8)|(HEAPTRACK_VERSION_PATCH))

#define HEAPTRACK_FILE_FORMAT_VERSION @HEAPTRACK_FILE_FORMAT_VERSION@

#define HEAPTRACK_DEBUG_BUILD @HEAPTRACK_DEBUG_BUILD@

// cfree() does not exist in glibc 2.26+.
// See: https://bugs.kde.org/show_bug.cgi?id=383889
#cmakedefine01 HAVE_CFREE
#cmakedefine01 HAVE_VALLOC

#endif // HEAPTRACK_CONFIG_H

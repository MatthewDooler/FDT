/*
 * 'rebel' branch modifications:
 *     Copyright (C) 2010 Tuxera. All Rights Reserved.
 */

/*
 * Copyright (C) 2006-2008 Google. All Rights Reserved.
 * Amit Singh <singh@>
 */

#ifndef _FUSE_VERSION_H_
#define _FUSE_VERSION_H_

#define OSXFUSE_STRINGIFY(s)         OSXFUSE_STRINGIFY_BACKEND(s)
#define OSXFUSE_STRINGIFY_BACKEND(s) #s

/* Add things here. */

#define OSXFUSE_FS_TYPE_LITERAL osxfusefs
#define OSXFUSE_FS_TYPE         OSXFUSE_STRINGIFY(OSXFUSE_FS_TYPE_LITERAL)

#define OSXFUSE_IDENTIFIER_LITERAL com.github.osxfuse
#define OSXFUSE_IDENTIFIER OSXFUSE_STRINGIFY(OSXFUSE_IDENTIFIER_LITERAL)

#define OSXFUSE_BUNDLE_IDENTIFIER_LITERAL \
        OSXFUSE_IDENTIFIER_LITERAL.filesystems.OSXFUSE_FS_TYPE_LITERAL
#define OSXFUSE_BUNDLE_IDENTIFIER \
        OSXFUSE_STRINGIFY(OSXFUSE_BUNDLE_IDENTIFIER_LITERAL)

#define OSXFUSE_BUNDLE_IDENTIFIER_TRUNK_LITERAL  osxfusefs
#define OSXFUSE_BUNDLE_IDENTIFIER_TRUNK \
        OSXFUSE_STRINGIFY(OSXFUSE_BUNDLE_IDENTIFIER_TRUNK_LITERAL)

#define OSXFUSE_TIMESTAMP __DATE__ ", " __TIME__

#define OSXFUSE_VERSION_LITERAL 2.6.1
#define OSXFUSE_VERSION         OSXFUSE_STRINGIFY(OSXFUSE_VERSION_LITERAL)

#define FUSE_KPI_GEQ(M, m) \
    (FUSE_KERNEL_VERSION > (M) || \
    (FUSE_KERNEL_VERSION == (M) && FUSE_KERNEL_MINOR_VERSION >= (m)))

#endif /* _FUSE_VERSION_H_ */
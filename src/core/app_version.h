#ifndef APP_VERSION_H
#define APP_VERSION_H

/* Human-facing release identity, deliberately separate from BUILD_STAMP.
 * See app_version.c: its target object is rebuilt on every make invocation so
 * switching RELEASE_LABEL cannot leave an incremental build with stale text. */
const char * app_version_label(void);
const char * app_build_identifier(void);

#endif

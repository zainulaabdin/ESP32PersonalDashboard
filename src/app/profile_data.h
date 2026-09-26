#pragma once

// Profile tab content. Put your own in src/app/profile_private/ (gitignored):
// profile_private.h with the PROFILE_* strings below, plus profile_pic.c /
// qr_small.c / qr_large.c images (180x180, 56x56, 230x230). Release builds
// always use these placeholders, so published firmware has no personal data.
#if !defined(RELEASE_BUILD) && __has_include("profile_private/profile_private.h")
#define PROFILE_PRIVATE 1
#include "profile_private/profile_private.h"
#else
#define PROFILE_PRIVATE 0
#define PROFILE_NAME "Your Name"
#define PROFILE_TITLE "Job title"
#define PROFILE_ORG "Organisation, City"
#define PROFILE_LINE1 "Speciality one"
#define PROFILE_LINE2 "Speciality two"
#define PROFILE_LINE3 "Speciality three"
#define PROFILE_PHONE "(+00) 0000 0000"
#define PROFILE_EMAIL "you@example.com"
#define PROFILE_WEB "www.example.com"
#endif

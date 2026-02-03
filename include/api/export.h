#ifndef ACCT_EXPORT_H
#define ACCT_EXPORT_H

#if defined(_WIN32) || defined(__CYGWIN__)
    #ifdef ACCT_API_EXPORT
        #define ACCT_API __declspec(dllexport)
    #else
        #define ACCT_API __declspec(dllimport)
    #endif
#else
    #ifdef ACCT_API_EXPORT
        #define ACCT_API __attribute__((visibility("default")))
    #else
        #define ACCT_API
    #endif
#endif

#endif // ACCT_EXPORT_H
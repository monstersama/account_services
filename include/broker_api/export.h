#ifndef ACCT_BROKER_API_EXPORT_H
#define ACCT_BROKER_API_EXPORT_H

#if defined(_WIN32) || defined(__CYGWIN__)
#ifdef ACCT_BROKER_API_EXPORT
#define ACCT_BROKER_API __declspec(dllexport)
#else
#define ACCT_BROKER_API __declspec(dllimport)
#endif
#else
#ifdef ACCT_BROKER_API_EXPORT
#define ACCT_BROKER_API __attribute__((visibility("default")))
#else
#define ACCT_BROKER_API
#endif
#endif

#endif

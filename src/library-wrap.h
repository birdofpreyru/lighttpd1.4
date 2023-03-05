#ifndef LIBRARY_WRAP_H
#define LIBRARY_WRAP_H

#include "first.h"
#include "base.h"
#include <signal.h>

extern volatile sig_atomic_t graceful_shutdown;
static void server_main_loop (server * const srv);

#if defined(HAVE_SYSLOG_H) && defined(WITH_ANDROID_NDK_SYSLOG_INTERCEPT)

#include <stdarg.h>
#include <syslog.h>
#include <android/log.h>

static const char *android_log_tag = "lighttpd";

void openlog(const char *ident, int option, int facility)
{
    android_log_tag = ident;/*(configfile.c passes persistent constant string)*/
    UNUSED(option);
    UNUSED(facility);
}

void closelog(void)
{
}

void syslog(int priority, const char *format, ...)
{
    switch (priority) {
      case LOG_EMERG:   priority = ANDROID_LOG_FATAL; break;
      case LOG_ALERT:   priority = ANDROID_LOG_FATAL; break;
      case LOG_CRIT:    priority = ANDROID_LOG_ERROR; break;
      case LOG_ERR:     priority = ANDROID_LOG_ERROR; break;
      case LOG_WARNING: priority = ANDROID_LOG_WARN;  break;
      case LOG_NOTICE:  priority = ANDROID_LOG_INFO;  break;
      case LOG_INFO:    priority = ANDROID_LOG_INFO;  break;
      case LOG_DEBUG:   priority = ANDROID_LOG_DEBUG; break;
      default:          priority = ANDROID_LOG_ERROR; break;
    }

    va_list ap;
    va_start(ap, format);
    __android_log_vprint(priority, android_log_tag, format, ap);
    va_end(ap);
}

#endif /* HAVE_SYSLOG_H && WITH_ANDROID_NDK_SYSLOG_INTERCEPT */

#ifdef WITH_JAVA_NATIVE_INTERFACE

#include <jni.h>

int lighttpd_jni_main (int argc, char ** argv, JNIEnv *jenv);
#define main(a,b) \
  lighttpd_jni_main (int argc, char ** argv, JNIEnv *jenv)

static void lighttpd_jni_main_loop (server * const srv, JNIEnv *jenv)
{
    jclass ServerClass = (*jenv)->FindClass(jenv, "com/lighttpd/Server");
    if (ServerClass) {
        jmethodID onLaunchedID = (*jenv)->GetStaticMethodID(
            jenv, ServerClass, "onLaunchedCallback", "()V");
        if (onLaunchedID)
            (*jenv)->CallStaticVoidMethod(jenv, ServerClass, onLaunchedID);
    }
    server_main_loop(srv);
}
#define server_main_loop(srv) lighttpd_jni_main_loop((srv), jenv);

__attribute_cold__
JNIEXPORT jint JNICALL Java_com_lighttpd_Server_launch(
    JNIEnv *jenv,
    jobject thisObject,
    jstring configPath
) {
    UNUSED(thisObject);

    const char * config_path = (*jenv)->GetStringUTFChars(jenv, configPath, 0);
    if (!config_path) return -1;

    optind = 1;
    char *argv[] = { "lighttpd", "-D", "-f", (char*)config_path, NULL };
    int rc = lighttpd_jni_main(4, argv, jenv);

    (*jenv)->ReleaseStringUTFChars(jenv, configPath, config_path);
    return rc;
}

__attribute_cold__
JNIEXPORT void JNICALL Java_com_lighttpd_Server_gracefulShutdown(
    JNIEnv *jenv,
    jobject thisObject
) {
    UNUSED(jenv);
    UNUSED(thisObject);
    graceful_shutdown = 1;
}

#else

// NOTE: lighttpd_main is occupied in server_win32.c file,
// thus we need to use a different name here to avoid conflicts
// in Windows (MinGW) build.

#ifndef _WIN32
int lighttpd_main (int argc, char ** argv, void (*callback)());
#endif

#define main(a,b) lighttpd_main (int argc, char ** argv, void (*callback)())

static void lighttpd_main_loop (server * const srv, void (*callback)())
{
    if (callback) callback();
    server_main_loop(srv);
}
#define server_main_loop(srv) lighttpd_main_loop((srv), callback);

__attribute_cold__
int lighttpd_launch(
    const char * config_path,
    const char * modules_path, // TODO: This should be wired up here,
                               // and in native and JS sides for all platforms.
    void (*callback)()
) {
    /*
    AllocConsole();
    freopen("CONOUT$", "w", stdout);
    freopen("CONOUT$", "w", stderr);
    */

    if (!config_path) return -1;

    optind = 1;

    char *argv[] = {
        "lighttpd",
        "-D",
        "-f", (char*)config_path,
        NULL, NULL, NULL
    };

    int argc = 4;
    if (modules_path) {
        argv[4] = "-m";
        argv[5] = (char*)modules_path;
        argc = 6;
    }

    #ifdef _WIN32
        return lighttpd_win_main(argc, argv, callback);
    #else
        return lighttpd_main(argc, argv, callback);
    #endif
}

void lighttpd_graceful_shutdown() {
  graceful_shutdown = 1;
}

#endif

#endif /* LIBRARY_WRAP_H */

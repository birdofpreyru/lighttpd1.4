#ifndef LIBRARY_WRAP_H
#define LIBRARY_WRAP_H

#include "first.h"
#include "base.h"
#include <signal.h>
#include <stdio.h>

// BEWARE: These errlog_to_file() and errlog_to_stderr(), presumably, duplicate
// functionality in the normal Lighttpd code, and probably may conflict with it,
// but for our library scenario we need to do this error log redirection to file
// as early as possible, if it is opted; we don't wanna rely on the normal
// Lighttpd code to setup the logging, as before that we can miss all sorts of
// errors during the Lighttpd server configuration, and we want those errors,
// if we opt for the log file.

FILE *errlog = 0;
int original_stderr = -1; // -1 means stderr is not redirected.

/**
 * @brief Redirects the standard error stream to the specified file, and saves
 * the original stderr file descriptor to orignal_stderr variable to allow
 * restoring stderr later.
 * @param path
 */
void errlog_to_file(const char *path) {
  if (original_stderr == -1) {
    original_stderr = fileno(stderr);
    errlog = fopen(path, "a");
    dup2(fileno(errlog), fileno(stderr));
  }
}

/**
 * @brief Restores the original standard error log stream; noop if it is not
 * redirected.
 */
void errlog_to_stderr() {
  if (original_stderr != -1) {
    fflush(stderr);
    fclose(errlog);
    errlog = 0;
    dup2(original_stderr, fileno(stderr));
    original_stderr = -1;
  }
}

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

#define main // To prevent renaming of server_main() into main() in server.c

#ifdef WITH_JAVA_NATIVE_INTERFACE

#include <jni.h>

static void server_status_running (JNIEnv *jenv)
{
    jclass ServerClass = (*jenv)->FindClass(jenv, "com/lighttpd/Server");
    if (ServerClass) {
        jmethodID onLaunchedID = (*jenv)->GetStaticMethodID(
            jenv, ServerClass, "onLaunchedCallback", "()V");
        if (onLaunchedID)
            (*jenv)->CallStaticVoidMethod(jenv, ServerClass, onLaunchedID);
    }
}
#define server_status_running(srv) server_status_running(jenv);

int server_main(int argc, char ** argv, JNIEnv *jenv);

/**
 * @brief Launches the server in JNI use case.
 *
 * @param jenv
 * @param thisObject
 * @param configPath
 * @param errlogPath Optional. If given, the stderr stream will be redirected
 *  to the specified file the first thing in the launch flow. Otherwise, it will
 *  restore stderr to the original state, if needed.
 */
__attribute_cold__
JNIEXPORT jint JNICALL Java_com_lighttpd_Server_launch(
    JNIEnv *jenv,
    jobject thisObject,
    jstring configPath,
    jstring errlogPath
) {
    UNUSED(thisObject);

    const char * errlog_path = (*jenv)->GetStringUTFChars(jenv, errlogPath, 0);
    if (errlog_path[0] != '\0') errlog_to_file(errlog_path);
    else errlog_to_stderr();
    (*jenv)->ReleaseStringUTFChars(jenv, errlogPath, errlog_path);

    const char * config_path = (*jenv)->GetStringUTFChars(jenv, configPath, 0);
    if (!config_path) return -1;

    optind = 1;
    char *argv[] = { "lighttpd", "-D", "-f", (char*)config_path, NULL };
    int rc = server_main(4, argv, jenv);

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

#define server_main(a,b) server_main(a, b, JNIEnv *jenv)

#else

int server_main_win32(int argc, char ** argv, void (*callback)());

#ifndef _WIN32 // NOTE: For Win32 it is defined inside server_win32.c
int server_main(int argc, char ** argv, void (*callback)());
static void server_status_running (void (*callback)())
{
    if (callback) callback();
}
#define server_status_running(srv) server_status_running(callback);
#endif

__attribute_cold__
int lighttpd_launch(
  const char * config_path,
  const char * modules_path,
  const char * errlog_path,
  void (*callback)()
) {
    if (errlog_path && errlog_path[0] != '\0') errlog_to_file(errlog_path);
    else errlog_to_stderr();

    /* TODO: On Win32 it may be used to create / connect to the console.
    AllocConsole();
    freopen("CONOUT$", "w", stdout);
    freopen("CONOUT$", "w", stderr);
    */

    if (!config_path) return -1;

    optind = 1;
    int argc = 4;
    char *argv[] = { "lighttpd", "-D", "-f", (char*)config_path, NULL, NULL, NULL };

    if (modules_path) {
      argv[4] = "-m";
      argv[5] = (char*)modules_path;
      argc = 6;
    }

  #ifdef _WIN32
  return server_main_win32(argc, argv, callback);
  #else
	return server_main(argc, argv, callback);
  #endif
}

// NOTE: In WIN32 case it should be defined
// after the code in server_win32.c
#ifndef _WIN32
#define server_main(a,b) server_main(a, b, void (*callback)())
#endif

void lighttpd_graceful_shutdown() {
  graceful_shutdown = 1;
}

#endif

#endif /* LIBRARY_WRAP_H */

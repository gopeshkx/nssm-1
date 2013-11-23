#ifndef REGISTRY_H
#define REGISTRY_H

#define NSSM_REGISTRY _T("SYSTEM\\CurrentControlSet\\Services\\%s\\Parameters")
#define NSSM_REG_EXE _T("Application")
#define NSSM_REG_FLAGS _T("AppParameters")
#define NSSM_REG_DIR _T("AppDirectory")
#define NSSM_REG_ENV _T("AppEnvironment")
#define NSSM_REG_ENV_EXTRA _T("AppEnvironmentExtra")
#define NSSM_REG_EXIT _T("AppExit")
#define NSSM_REG_THROTTLE _T("AppThrottle")
#define NSSM_REG_STOP_METHOD_SKIP _T("AppStopMethodSkip")
#define NSSM_REG_KILL_CONSOLE_GRACE_PERIOD _T("AppStopMethodConsole")
#define NSSM_REG_KILL_WINDOW_GRACE_PERIOD _T("AppStopMethodWindow")
#define NSSM_REG_KILL_THREADS_GRACE_PERIOD _T("AppStopMethodThreads")
#define NSSM_REG_STDIN _T("AppStdin")
#define NSSM_REG_STDOUT _T("AppStdout")
#define NSSM_REG_STDERR _T("AppStderr")
#define NSSM_REG_STDIO_SHARING _T("ShareMode")
#define NSSM_REG_STDIO_DISPOSITION _T("CreationDisposition")
#define NSSM_REG_STDIO_FLAGS _T("FlagsAndAttributes")
#define NSSM_STDIO_LENGTH 29

int create_messages();
int create_parameters(nssm_service_t *);
int create_exit_action(TCHAR *, const TCHAR *);
int set_environment(TCHAR *, HKEY, TCHAR *, TCHAR **, unsigned long *);
int expand_parameter(HKEY, TCHAR *, TCHAR *, unsigned long, bool, bool);
int expand_parameter(HKEY, TCHAR *, TCHAR *, unsigned long, bool);
int set_expand_string(HKEY, TCHAR *, TCHAR *);
int set_number(HKEY, TCHAR *, unsigned long);
int get_number(HKEY, TCHAR *, unsigned long *, bool);
int get_number(HKEY, TCHAR *, unsigned long *);
void override_milliseconds(TCHAR *, HKEY, TCHAR *, unsigned long *, unsigned long, unsigned long);
int get_parameters(nssm_service_t *, STARTUPINFO *);
int get_exit_action(TCHAR *, unsigned long *, TCHAR *, bool *);

#endif

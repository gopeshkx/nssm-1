#include "nssm.h"

bool is_admin;
bool use_critical_section;

extern imports_t imports;

const TCHAR *exit_action_strings[] = { _T("Restart"), _T("Ignore"), _T("Exit"), _T("Suicide"), 0 };

static inline int throttle_milliseconds(unsigned long throttle) {
  /* pow() operates on doubles. */
  int ret = 1; for (unsigned long i = 1; i < throttle; i++) ret *= 2;
  return ret * 1000;
}

/*
  Wrapper to be called in a new thread so that we can acknowledge a STOP
  control immediately.
*/
static unsigned long WINAPI shutdown_service(void *arg) {
  return stop_service((nssm_service_t *) arg, 0, true, true);
}

/* Connect to the service manager */
SC_HANDLE open_service_manager() {
  SC_HANDLE ret = OpenSCManager(0, SERVICES_ACTIVE_DATABASE, SC_MANAGER_ALL_ACCESS);
  if (! ret) {
    if (is_admin) log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OPENSCMANAGER_FAILED, 0);
    return 0;
  }

  return ret;
}

/* Set default values which aren't zero. */
void set_nssm_service_defaults(nssm_service_t *service) {
  if (! service) return;

  service->stdin_sharing = NSSM_STDIN_SHARING;
  service->stdin_disposition = NSSM_STDIN_DISPOSITION;
  service->stdin_flags = NSSM_STDIN_FLAGS;
  service->stdout_sharing = NSSM_STDOUT_SHARING;
  service->stdout_disposition = NSSM_STDOUT_DISPOSITION;
  service->stdout_flags = NSSM_STDOUT_FLAGS;
  service->stderr_sharing = NSSM_STDERR_SHARING;
  service->stderr_disposition = NSSM_STDERR_DISPOSITION;
  service->stderr_flags = NSSM_STDERR_FLAGS;
  service->throttle_delay = NSSM_RESET_THROTTLE_RESTART;
  service->stop_method = ~0;
  service->kill_console_delay = NSSM_KILL_CONSOLE_GRACE_PERIOD;
  service->kill_window_delay = NSSM_KILL_WINDOW_GRACE_PERIOD;
  service->kill_threads_delay = NSSM_KILL_THREADS_GRACE_PERIOD;
}

/* Allocate and zero memory for a service. */
nssm_service_t *alloc_nssm_service() {
  nssm_service_t *service = (nssm_service_t *) HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(nssm_service_t));
  if (! service) log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OUT_OF_MEMORY, _T("service"), _T("alloc_nssm_service()"), 0);
  return service;
}

/* Free memory for a service. */
void cleanup_nssm_service(nssm_service_t *service) {
  if (! service) return;
  if (service->env) HeapFree(GetProcessHeap(), 0, service->env);
  if (service->env_extra) HeapFree(GetProcessHeap(), 0, service->env_extra);
  if (service->handle) CloseServiceHandle(service->handle);
  if (service->process_handle) CloseHandle(service->process_handle);
  if (service->wait_handle) UnregisterWait(service->process_handle);
  if (service->throttle_section_initialised) DeleteCriticalSection(&service->throttle_section);
  if (service->throttle_timer) CloseHandle(service->throttle_timer);
  HeapFree(GetProcessHeap(), 0, service);
}

/* About to install the service */
int pre_install_service(int argc, TCHAR **argv) {
  /* Show the dialogue box if we didn't give the service name and path */
  if (argc < 2) return nssm_gui(IDD_INSTALL, argv[0]);

  nssm_service_t *service = alloc_nssm_service();
  if (! service) {
    print_message(stderr, NSSM_EVENT_OUT_OF_MEMORY, _T("service"), _T("pre_install_service()"));
    return 1;
  }

  set_nssm_service_defaults(service);
  _sntprintf_s(service->name, _countof(service->name), _TRUNCATE, _T("%s"), argv[0]);
  _sntprintf_s(service->exe, _countof(service->exe), _TRUNCATE, _T("%s"), argv[1]);

  /* Arguments are optional */
  size_t flagslen = 0;
  size_t s = 0;
  int i;
  for (i = 2; i < argc; i++) flagslen += _tcslen(argv[i]) + 1;
  if (! flagslen) flagslen = 1;
  if (flagslen > _countof(service->flags)) {
    print_message(stderr, NSSM_MESSAGE_FLAGS_TOO_LONG);
    return 2;
  }

  for (i = 2; i < argc; i++) {
    size_t len = _tcslen(argv[i]);
    memmove(service->flags + s, argv[i], len * sizeof(TCHAR));
    s += len;
    if (i < argc - 1) service->flags[s++] = _T(' ');
  }

  /* Work out directory name */
  _sntprintf_s(service->dir, _countof(service->dir), _TRUNCATE, _T("%s"), service->exe);
  strip_basename(service->dir);

  int ret = install_service(service);
  cleanup_nssm_service(service);
  return ret;
}

/* About to remove the service */
int pre_remove_service(int argc, TCHAR **argv) {
  /* Show dialogue box if we didn't pass service name and "confirm" */
  if (argc < 2) return nssm_gui(IDD_REMOVE, argv[0]);
  if (str_equiv(argv[1], _T("confirm"))) {
    nssm_service_t *service = alloc_nssm_service();
    _sntprintf_s(service->name, _countof(service->name), _TRUNCATE, _T("%s"), argv[0]);
    int ret = remove_service(service);
    cleanup_nssm_service(service);
    return ret;
  }
  print_message(stderr, NSSM_MESSAGE_PRE_REMOVE_SERVICE);
  return 100;
}

/* Install the service */
int install_service(nssm_service_t *service) {
  if (! service) return 1;

  /* Open service manager */
  SC_HANDLE services = open_service_manager();
  if (! services) {
    print_message(stderr, NSSM_MESSAGE_OPEN_SERVICE_MANAGER_FAILED);
    cleanup_nssm_service(service);
    return 2;
  }

  /* Get path of this program */
  TCHAR path[MAX_PATH];
  GetModuleFileName(0, path, MAX_PATH);

  /* Construct command */
  TCHAR command[CMD_LENGTH];
  size_t pathlen = _tcslen(path);
  if (pathlen + 1 >= VALUE_LENGTH) {
    print_message(stderr, NSSM_MESSAGE_PATH_TOO_LONG, NSSM);
    return 3;
  }
  if (_sntprintf_s(command, sizeof(command), _TRUNCATE, _T("\"%s\""), path) < 0) {
    print_message(stderr, NSSM_MESSAGE_OUT_OF_MEMORY_FOR_IMAGEPATH);
    return 4;
  }

  /* Create the service */
  service->handle = CreateService(services, service->name, service->name, SC_MANAGER_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, command, 0, 0, 0, 0, 0);
  if (! service->handle) {
    print_message(stderr, NSSM_MESSAGE_CREATESERVICE_FAILED);
    CloseServiceHandle(services);
    return 5;
  }

  /* Now we need to put the parameters into the registry */
  if (create_parameters(service)) {
    print_message(stderr, NSSM_MESSAGE_CREATE_PARAMETERS_FAILED);
    DeleteService(service->handle);
    CloseServiceHandle(services);
    return 6;
  }

  set_service_recovery(service);

  print_message(stdout, NSSM_MESSAGE_SERVICE_INSTALLED, service->name);

  /* Cleanup */
  CloseServiceHandle(services);

  return 0;
}

/* Remove the service */
int remove_service(nssm_service_t *service) {
  if (! service) return 1;

  /* Open service manager */
  SC_HANDLE services = open_service_manager();
  if (! services) {
    print_message(stderr, NSSM_MESSAGE_OPEN_SERVICE_MANAGER_FAILED);
    return 2;
  }

  /* Try to open the service */
  service->handle = OpenService(services, service->name, SC_MANAGER_ALL_ACCESS);
  if (! service->handle) {
    print_message(stderr, NSSM_MESSAGE_OPENSERVICE_FAILED);
    CloseServiceHandle(services);
    return 3;
  }

  /* Try to delete the service */
  if (! DeleteService(service->handle)) {
    print_message(stderr, NSSM_MESSAGE_DELETESERVICE_FAILED);
    CloseServiceHandle(services);
    return 4;
  }

  /* Cleanup */
  CloseServiceHandle(services);

  print_message(stdout, NSSM_MESSAGE_SERVICE_REMOVED, service->name);
  return 0;
}

/* Service initialisation */
void WINAPI service_main(unsigned long argc, TCHAR **argv) {
  nssm_service_t *service = alloc_nssm_service();
  if (! service) return;

  if (_sntprintf_s(service->name, _countof(service->name), _TRUNCATE, _T("%s"), argv[0]) < 0) {
    log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OUT_OF_MEMORY, _T("service->name"), _T("service_main()"), 0);
    return;
  }

  /* We can use a condition variable in a critical section on Vista or later. */
  if (imports.SleepConditionVariableCS && imports.WakeConditionVariable) use_critical_section = true;
  else use_critical_section = false;

  /* Initialise status */
  ZeroMemory(&service->status, sizeof(service->status));
  service->status.dwServiceType = SERVICE_WIN32_OWN_PROCESS | SERVICE_INTERACTIVE_PROCESS;
  service->status.dwControlsAccepted = SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_PAUSE_CONTINUE;
  service->status.dwWin32ExitCode = NO_ERROR;
  service->status.dwServiceSpecificExitCode = 0;
  service->status.dwCheckPoint = 0;
  service->status.dwWaitHint = NSSM_WAITHINT_MARGIN;

  /* Signal we AREN'T running the server */
  service->process_handle = 0;
  service->pid = 0;

  /* Register control handler */
  service->status_handle = RegisterServiceCtrlHandlerEx(NSSM, service_control_handler, (void *) service);
  if (! service->status_handle) {
    log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_REGISTERSERVICECTRLHANDER_FAILED, error_string(GetLastError()), 0);
    return;
  }

  log_service_control(service->name, 0, true);

  service->status.dwCurrentState = SERVICE_START_PENDING;
  service->status.dwWaitHint = service->throttle_delay + NSSM_WAITHINT_MARGIN;
  SetServiceStatus(service->status_handle, &service->status);

  if (is_admin) {
    /* Try to create the exit action parameters; we don't care if it fails */
    create_exit_action(service->name, exit_action_strings[0]);

    SC_HANDLE services = open_service_manager();
    if (services) {
      service->handle = OpenService(services, service->name, SC_MANAGER_ALL_ACCESS);
      set_service_recovery(service);
      CloseServiceHandle(services);
    }
  }

  /* Used for signalling a resume if the service pauses when throttled. */
  if (use_critical_section) {
    InitializeCriticalSection(&service->throttle_section);
    service->throttle_section_initialised = true;
  }
  else {
    service->throttle_timer = CreateWaitableTimer(0, 1, 0);
    if (! service->throttle_timer) {
      log_event(EVENTLOG_WARNING_TYPE, NSSM_EVENT_CREATEWAITABLETIMER_FAILED, service->name, error_string(GetLastError()), 0);
    }
  }

  monitor_service(service);
}

/* Make sure service recovery actions are taken where necessary */
void set_service_recovery(nssm_service_t *service) {
  SERVICE_FAILURE_ACTIONS_FLAG flag;
  ZeroMemory(&flag, sizeof(flag));
  flag.fFailureActionsOnNonCrashFailures = true;

  /* This functionality was added in Vista so the call may fail */
  if (! ChangeServiceConfig2(service->handle, SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, &flag)) {
    unsigned long error = GetLastError();
    /* Pre-Vista we expect to fail with ERROR_INVALID_LEVEL */
    if (error != ERROR_INVALID_LEVEL) {
      log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_CHANGESERVICECONFIG2_FAILED, service->name, error_string(error), 0);
    }
  }
}

int monitor_service(nssm_service_t *service) {
  /* Set service status to started */
  int ret = start_service(service);
  if (ret) {
    TCHAR code[16];
    _sntprintf_s(code, _countof(code), _TRUNCATE, _T("%d"), ret);
    log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_START_SERVICE_FAILED, service->exe, service->name, ret, 0);
    return ret;
  }
  log_event(EVENTLOG_INFORMATION_TYPE, NSSM_EVENT_STARTED_SERVICE, service->exe, service->flags, service->name, service->dir, 0);

  /* Monitor service */
  if (! RegisterWaitForSingleObject(&service->wait_handle, service->process_handle, end_service, (void *) service, INFINITE, WT_EXECUTEONLYONCE | WT_EXECUTELONGFUNCTION)) {
    log_event(EVENTLOG_WARNING_TYPE, NSSM_EVENT_REGISTERWAITFORSINGLEOBJECT_FAILED, service->name, service->exe, error_string(GetLastError()), 0);
  }

  return 0;
}

TCHAR *service_control_text(unsigned long control) {
  switch (control) {
    /* HACK: there is no SERVICE_CONTROL_START constant */
    case 0: return _T("START");
    case SERVICE_CONTROL_STOP: return _T("STOP");
    case SERVICE_CONTROL_SHUTDOWN: return _T("SHUTDOWN");
    case SERVICE_CONTROL_PAUSE: return _T("PAUSE");
    case SERVICE_CONTROL_CONTINUE: return _T("CONTINUE");
    case SERVICE_CONTROL_INTERROGATE: return _T("INTERROGATE");
    default: return 0;
  }
}

void log_service_control(TCHAR *service_name, unsigned long control, bool handled) {
  TCHAR *text = service_control_text(control);
  unsigned long event;

  if (! text) {
    /* "0x" + 8 x hex + NULL */
    text = (TCHAR *) HeapAlloc(GetProcessHeap(), 0, 11 * sizeof(TCHAR));
    if (! text) {
      log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OUT_OF_MEMORY, _T("control code"), _T("log_service_control()"), 0);
      return;
    }
    if (_sntprintf_s(text, 11, _TRUNCATE, _T("0x%08x"), control) < 0) {
      log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OUT_OF_MEMORY, _T("control code"), _T("log_service_control()"), 0);
      HeapFree(GetProcessHeap(), 0, text);
      return;
    }

    event = NSSM_EVENT_SERVICE_CONTROL_UNKNOWN;
  }
  else if (handled) event = NSSM_EVENT_SERVICE_CONTROL_HANDLED;
  else event = NSSM_EVENT_SERVICE_CONTROL_NOT_HANDLED;

  log_event(EVENTLOG_INFORMATION_TYPE, event, service_name, text, 0);

  if (event == NSSM_EVENT_SERVICE_CONTROL_UNKNOWN) {
    HeapFree(GetProcessHeap(), 0, text);
  }
}

/* Service control handler */
unsigned long WINAPI service_control_handler(unsigned long control, unsigned long event, void *data, void *context) {
  nssm_service_t *service = (nssm_service_t *) context;

  switch (control) {
    case SERVICE_CONTROL_INTERROGATE:
      /* We always keep the service status up-to-date so this is a no-op. */
      return NO_ERROR;

    case SERVICE_CONTROL_SHUTDOWN:
    case SERVICE_CONTROL_STOP:
      log_service_control(service->name, control, true);
      /*
        We MUST acknowledge the stop request promptly but we're committed to
        waiting for the application to exit.  Spawn a new thread to wait
        while we acknowledge the request.
      */
      if (! CreateThread(NULL, 0, shutdown_service, context, 0, NULL)) {
        log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_CREATETHREAD_FAILED, error_string(GetLastError()), 0);

        /*
          We couldn't create a thread to tidy up so we'll have to force the tidyup
          to complete in time in this thread.
        */
        service->kill_console_delay = NSSM_KILL_CONSOLE_GRACE_PERIOD;
        service->kill_window_delay = NSSM_KILL_WINDOW_GRACE_PERIOD;
        service->kill_threads_delay = NSSM_KILL_THREADS_GRACE_PERIOD;

        stop_service(service, 0, true, true);
      }
      return NO_ERROR;

    case SERVICE_CONTROL_CONTINUE:
      log_service_control(service->name, control, true);
      service->throttle = 0;
      if (use_critical_section) imports.WakeConditionVariable(&service->throttle_condition);
      else {
        if (! service->throttle_timer) return ERROR_CALL_NOT_IMPLEMENTED;
        ZeroMemory(&service->throttle_duetime, sizeof(service->throttle_duetime));
        SetWaitableTimer(service->throttle_timer, &service->throttle_duetime, 0, 0, 0, 0);
      }
      service->status.dwCurrentState = SERVICE_CONTINUE_PENDING;
      service->status.dwWaitHint = throttle_milliseconds(service->throttle) + NSSM_WAITHINT_MARGIN;
      log_event(EVENTLOG_INFORMATION_TYPE, NSSM_EVENT_RESET_THROTTLE, service->name, 0);
      SetServiceStatus(service->status_handle, &service->status);
      return NO_ERROR;

    case SERVICE_CONTROL_PAUSE:
      /*
        We don't accept pause messages but it isn't possible to register
        only for continue messages so we have to handle this case.
      */
      log_service_control(service->name, control, false);
      return ERROR_CALL_NOT_IMPLEMENTED;
  }

  /* Unknown control */
  log_service_control(service->name, control, false);
  return ERROR_CALL_NOT_IMPLEMENTED;
}

/* Start the service */
int start_service(nssm_service_t *service) {
  service->stopping = false;
  service->allow_restart = true;

  if (service->process_handle) return 0;

  /* Allocate a STARTUPINFO structure for a new process */
  STARTUPINFO si;
  ZeroMemory(&si, sizeof(si));
  si.cb = sizeof(si);

  /* Allocate a PROCESSINFO structure for the process */
  PROCESS_INFORMATION pi;
  ZeroMemory(&pi, sizeof(pi));

  /* Get startup parameters */
  int ret = get_parameters(service, &si);
  if (ret) {
    log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_GET_PARAMETERS_FAILED, service->name, 0);
    return stop_service(service, 2, true, true);
  }

  /* Launch executable with arguments */
  TCHAR cmd[CMD_LENGTH];
  if (_sntprintf_s(cmd, _countof(cmd), _TRUNCATE, _T("\"%s\" %s"), service->exe, service->flags) < 0) {
    log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OUT_OF_MEMORY, _T("command line"), _T("start_service"), 0);
    close_output_handles(&si);
    return stop_service(service, 2, true, true);
  }

  throttle_restart(service);

  bool inherit_handles = false;
  if (si.dwFlags & STARTF_USESTDHANDLES) inherit_handles = true;
  unsigned long flags = 0;
#ifdef UNICODE
  flags |= CREATE_UNICODE_ENVIRONMENT;
#endif
  if (! CreateProcess(0, cmd, 0, 0, inherit_handles, flags, service->env, service->dir, &si, &pi)) {
    unsigned long error = GetLastError();
    if (error == ERROR_INVALID_PARAMETER && service->env) log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_CREATEPROCESS_FAILED_INVALID_ENVIRONMENT, service->name, service->exe, NSSM_REG_ENV, 0);
    else log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_CREATEPROCESS_FAILED, service->name, service->exe, error_string(error), 0);
    close_output_handles(&si);
    return stop_service(service, 3, true, true);
  }
  service->process_handle = pi.hProcess;
  service->pid = pi.dwProcessId;

  if (get_process_creation_time(service->process_handle, &service->creation_time)) ZeroMemory(&service->creation_time, sizeof(service->creation_time));

  close_output_handles(&si);

  /*
    Wait for a clean startup before changing the service status to RUNNING
    but be mindful of the fact that we are blocking the service control manager
    so abandon the wait before too much time has elapsed.
  */
  unsigned long delay = service->throttle_delay;
  if (delay > NSSM_SERVICE_STATUS_DEADLINE) {
    TCHAR delay_milliseconds[16];
    _sntprintf_s(delay_milliseconds, _countof(delay_milliseconds), _TRUNCATE, _T("%lu"), delay);
    TCHAR deadline_milliseconds[16];
    _sntprintf_s(deadline_milliseconds, _countof(deadline_milliseconds), _TRUNCATE, _T("%lu"), NSSM_SERVICE_STATUS_DEADLINE);
    log_event(EVENTLOG_WARNING_TYPE, NSSM_EVENT_STARTUP_DELAY_TOO_LONG, service->name, delay_milliseconds, NSSM, deadline_milliseconds, 0);
    delay = NSSM_SERVICE_STATUS_DEADLINE;
  }
  unsigned long deadline = WaitForSingleObject(service->process_handle, delay);

  /* Signal successful start */
  service->status.dwCurrentState = SERVICE_RUNNING;
  SetServiceStatus(service->status_handle, &service->status);

  /* Continue waiting for a clean startup. */
  if (deadline == WAIT_TIMEOUT) {
    if (service->throttle_delay > delay) {
      if (WaitForSingleObject(service->process_handle, service->throttle_delay - delay) == WAIT_TIMEOUT) service->throttle = 0;
    }
    else service->throttle = 0;
  }

  return 0;
}

/* Stop the service */
int stop_service(nssm_service_t *service, unsigned long exitcode, bool graceful, bool default_action) {
  service->allow_restart = false;
  if (service->wait_handle) {
    UnregisterWait(service->wait_handle);
    service->wait_handle = 0;
  }

  if (default_action && ! exitcode && ! graceful) {
    log_event(EVENTLOG_INFORMATION_TYPE, NSSM_EVENT_GRACEFUL_SUICIDE, service->name, service->exe, exit_action_strings[NSSM_EXIT_UNCLEAN], exit_action_strings[NSSM_EXIT_UNCLEAN], exit_action_strings[NSSM_EXIT_UNCLEAN], exit_action_strings[NSSM_EXIT_REALLY] ,0);
    graceful = true;
  }

  /* Signal we are stopping */
  if (graceful) {
    service->status.dwCurrentState = SERVICE_STOP_PENDING;
    service->status.dwWaitHint = NSSM_WAITHINT_MARGIN;
    SetServiceStatus(service->status_handle, &service->status);
  }

  /* Nothing to do if service isn't running */
  if (service->pid) {
    /* Shut down service */
    log_event(EVENTLOG_INFORMATION_TYPE, NSSM_EVENT_TERMINATEPROCESS, service->name, service->exe, 0);
    kill_process(service, service->process_handle, service->pid, 0);
  }
  else log_event(EVENTLOG_INFORMATION_TYPE, NSSM_EVENT_PROCESS_ALREADY_STOPPED, service->name, service->exe, 0);

  end_service((void *) service, true);

  /* Signal we stopped */
  if (graceful) {
    service->status.dwCurrentState = SERVICE_STOPPED;
    if (exitcode) {
      service->status.dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR;
      service->status.dwServiceSpecificExitCode = exitcode;
    }
    else {
      service->status.dwWin32ExitCode = NO_ERROR;
      service->status.dwServiceSpecificExitCode = 0;
    }
    SetServiceStatus(service->status_handle, &service->status);
  }

  return exitcode;
}

/* Callback function triggered when the server exits */
void CALLBACK end_service(void *arg, unsigned char why) {
  nssm_service_t *service = (nssm_service_t *) arg;

  if (service->stopping) return;

  service->stopping = true;

  /* Check exit code */
  unsigned long exitcode = 0;
  TCHAR code[16];
  GetExitCodeProcess(service->process_handle, &exitcode);
  if (exitcode == STILL_ACTIVE || get_process_exit_time(service->process_handle, &service->exit_time)) GetSystemTimeAsFileTime(&service->exit_time);
  CloseHandle(service->process_handle);

  service->process_handle = 0;
  service->pid = 0;

  /*
    Log that the service ended BEFORE logging about killing the process
    tree.  See below for the possible values of the why argument.
  */
  if (! why) {
    _sntprintf_s(code, _countof(code), _TRUNCATE, _T("%lu"), exitcode);
    log_event(EVENTLOG_INFORMATION_TYPE, NSSM_EVENT_ENDED_SERVICE, service->exe, service->name, code, 0);
  }

  /* Clean up. */
  if (exitcode == STILL_ACTIVE) exitcode = 0;
  kill_process_tree(service, service->pid, exitcode, service->pid);

  /*
    The why argument is true if our wait timed out or false otherwise.
    Our wait is infinite so why will never be true when called by the system.
    If it is indeed true, assume we were called from stop_service() because
    this is a controlled shutdown, and don't take any restart action.
  */
  if (why) return;
  if (! service->allow_restart) return;

  /* What action should we take? */
  int action = NSSM_EXIT_RESTART;
  TCHAR action_string[ACTION_LEN];
  bool default_action;
  if (! get_exit_action(service->name, &exitcode, action_string, &default_action)) {
    for (int i = 0; exit_action_strings[i]; i++) {
      if (! _tcsnicmp((const TCHAR *) action_string, exit_action_strings[i], ACTION_LEN)) {
        action = i;
        break;
      }
    }
  }

  switch (action) {
    /* Try to restart the service or return failure code to service manager */
    case NSSM_EXIT_RESTART:
      log_event(EVENTLOG_INFORMATION_TYPE, NSSM_EVENT_EXIT_RESTART, service->name, code, exit_action_strings[action], service->exe, 0);
      while (monitor_service(service)) {
        log_event(EVENTLOG_WARNING_TYPE, NSSM_EVENT_RESTART_SERVICE_FAILED, service->exe, service->name, 0);
        Sleep(30000);
      }
    break;

    /* Do nothing, just like srvany would */
    case NSSM_EXIT_IGNORE:
      log_event(EVENTLOG_INFORMATION_TYPE, NSSM_EVENT_EXIT_IGNORE, service->name, code, exit_action_strings[action], service->exe, 0);
      Sleep(INFINITE);
    break;

    /* Tell the service manager we are finished */
    case NSSM_EXIT_REALLY:
      log_event(EVENTLOG_INFORMATION_TYPE, NSSM_EVENT_EXIT_REALLY, service->name, code, exit_action_strings[action], 0);
      stop_service(service, exitcode, true, default_action);
    break;

    /* Fake a crash so pre-Vista service managers will run recovery actions. */
    case NSSM_EXIT_UNCLEAN:
      log_event(EVENTLOG_INFORMATION_TYPE, NSSM_EVENT_EXIT_UNCLEAN, service->name, code, exit_action_strings[action], 0);
      stop_service(service, exitcode, false, default_action);
      free_imports();
      exit(exitcode);
    break;
  }
}

void throttle_restart(nssm_service_t *service) {
  /* This can't be a restart if the service is already running. */
  if (! service->throttle++) return;

  int ms = throttle_milliseconds(service->throttle);

  if (service->throttle > 7) service->throttle = 8;

  TCHAR threshold[8], milliseconds[8];
  _sntprintf_s(threshold, _countof(threshold), _TRUNCATE, _T("%lu"), service->throttle_delay);
  _sntprintf_s(milliseconds, _countof(milliseconds), _TRUNCATE, _T("%lu"), ms);
  log_event(EVENTLOG_WARNING_TYPE, NSSM_EVENT_THROTTLED, service->name, threshold, milliseconds, 0);

  if (use_critical_section) EnterCriticalSection(&service->throttle_section);
  else if (service->throttle_timer) {
    ZeroMemory(&service->throttle_duetime, sizeof(service->throttle_duetime));
    service->throttle_duetime.QuadPart = 0 - (ms * 10000LL);
    SetWaitableTimer(service->throttle_timer, &service->throttle_duetime, 0, 0, 0, 0);
  }

  service->status.dwCurrentState = SERVICE_PAUSED;
  SetServiceStatus(service->status_handle, &service->status);

  if (use_critical_section) {
    imports.SleepConditionVariableCS(&service->throttle_condition, &service->throttle_section, ms);
    LeaveCriticalSection(&service->throttle_section);
  }
  else {
    if (service->throttle_timer) WaitForSingleObject(service->throttle_timer, INFINITE);
    else Sleep(ms);
  }
}

/*
  When responding to a stop (or any other) request we need to set dwWaitHint to
  the number of milliseconds we expect the operation to take, and optionally
  increase dwCheckPoint.  If dwWaitHint milliseconds elapses without the
  operation completing or dwCheckPoint increasing, the system will consider the
  service to be hung.

  However the system will consider the service to be hung after 30000
  milliseconds regardless of the value of dwWaitHint if dwCheckPoint has not
  changed.  Therefore if we want to wait longer than that we must periodically
  increase dwCheckPoint.

  Furthermore, it will consider the service to be hung after 60000 milliseconds
  regardless of the value of dwCheckPoint unless dwWaitHint is increased every
  time dwCheckPoint is also increased.

  Our strategy then is to retrieve the initial dwWaitHint and wait for
  NSSM_SERVICE_STATUS_DEADLINE milliseconds.  If the process is still running
  and we haven't finished waiting we increment dwCheckPoint and add whichever is
  smaller of NSSM_SERVICE_STATUS_DEADLINE or the remaining timeout to
  dwWaitHint.

  Only doing both these things will prevent the system from killing the service.

  Returns: 1 if the wait timed out.
           0 if the wait completed.
          -1 on error.
*/
int await_shutdown(nssm_service_t *service, TCHAR *function_name, unsigned long timeout) {
  unsigned long interval;
  unsigned long waithint;
  unsigned long ret;
  unsigned long waited;
  TCHAR interval_milliseconds[16];
  TCHAR timeout_milliseconds[16];
  TCHAR waited_milliseconds[16];
  TCHAR *function = function_name;

  /* Add brackets to function name. */
  size_t funclen = _tcslen(function_name) + 3;
  TCHAR *func = (TCHAR *) HeapAlloc(GetProcessHeap(), 0, funclen * sizeof(TCHAR));
  if (func) {
    if (_sntprintf_s(func, funclen, _TRUNCATE, _T("%s()"), function_name) > -1) function = func;
  }

  _sntprintf_s(timeout_milliseconds, _countof(timeout_milliseconds), _TRUNCATE, _T("%lu"), timeout);

  waithint = service->status.dwWaitHint;
  waited = 0;
  while (waited < timeout) {
    interval = timeout - waited;
    if (interval > NSSM_SERVICE_STATUS_DEADLINE) interval = NSSM_SERVICE_STATUS_DEADLINE;

    service->status.dwCurrentState = SERVICE_STOP_PENDING;
    service->status.dwWaitHint += interval;
    service->status.dwCheckPoint++;
    SetServiceStatus(service->status_handle, &service->status);

    if (waited) {
      _sntprintf_s(waited_milliseconds, _countof(waited_milliseconds), _TRUNCATE, _T("%lu"), waited);
      _sntprintf_s(interval_milliseconds, _countof(interval_milliseconds), _TRUNCATE, _T("%lu"), interval);
      log_event(EVENTLOG_INFORMATION_TYPE, NSSM_EVENT_AWAITING_SHUTDOWN, function, service->name, waited_milliseconds, interval_milliseconds, timeout_milliseconds, 0);
    }

    switch (WaitForSingleObject(service->process_handle, interval)) {
      case WAIT_OBJECT_0:
        ret = 0;
        goto awaited;

      case WAIT_TIMEOUT:
        ret = 1;
      break;

      default:
        ret = -1;
        goto awaited;
    }

    waited += interval;
  }

awaited:
  if (func) HeapFree(GetProcessHeap(), 0, func);

  return ret;
}

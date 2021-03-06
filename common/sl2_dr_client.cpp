#include "vendor/picosha2.h"

#include "common/sl2_dr_client.hpp"

using namespace std;

// NOTE(ww): As of Windows 10, both KERNEL32.dll and ADVAPI32.dll
// get forwarded to KERNELBASE.DLL, apparently.
// https://docs.microsoft.com/en-us/windows/desktop/Win7AppQual/new-low-level-binaries
// TODO(ww): Since we iterate over these, order them by likelihood of occurrence?
/** Maps functions to the DLL's we _expect_ them to appear in  */
sl2_funcmod SL2_FUNCMOD_TABLE[] = {
    {"ReadFile", "KERNELBASE.DLL"},
    {"recv", "WS2_32.DLL"},                     // TODO(ww): Is this right?
    {"WinHttpReadData", "WINHTTP.DLL"},         // TODO(ww): Is this right?
    {"InternetReadFile", "WININET.DLL"},        // TODO(ww): Is this right?
    {"WinHttpWebSocketReceive", "WINHTTP.DLL"}, // TODO(ww): Is this right?
    {"RegQueryValueExA", "KERNELBASE.DLL"},
    {"RegQueryValueExW", "KERNELBASE.DLL"},
    {"ReadEventLogA", "KERNELBASE.DLL"},
    {"ReadEventLogW", "KERNELBASE.DLL"},
    {"fread", "UCRTBASE.DLL"},
    {"fread", "UCRTBASED.DLL"},
    {"fread", "MSVCRT.DLL"},
    {"fread", "MSVCRTD.DLL"},
    {"fread_s", "UCRTBASE.DLL"},
    {"fread_s", "UCRTBASED.DLL"},
    {"fread_s", "MSVCRT.DLL"},
    {"fread_s", "MSVCRTD.DLL"},
    {"_read", "UCRTBASE.DLL"},
    {"_read", "UCRTBASED.DLL"},
    {"_read", "MSVCRT.DLL"},
    {"_read", "MSVCRTD.DLL"},
    {"MapViewOfFile", "KERNELBASE.DLL"},
};

// This should be moved inside
// the DR build process eventually and be the superclass of Fuzzer and Tracer subclasses.
/**
 * Common functionality for DynamoRIO clients
 */
SL2Client::SL2Client() {
}

/**
 * Creates a SHA256 hash of the arguments to a function
 * @param argHash pointer to the char buffer to write the hex-encoded hash into
 * @param hash_ctx pointer to the context containing the information to hash
 */
void SL2Client::hash_args(char *argHash, hash_context *hash_ctx) {
  std::vector<unsigned char> blob_vec((unsigned char *)hash_ctx,
                                      ((unsigned char *)hash_ctx) + sizeof(hash_context));
  std::string hash_str;
  picosha2::hash256_hex_string(blob_vec, hash_str);
  argHash[SL2_HASH_LEN] = 0;
  memcpy((void *)argHash, hash_str.c_str(), SL2_HASH_LEN);
}

/**
 * Implements targeting strategies to determine whether we should fuzz a given function call.
 * @param info - struct containing information about the last function call hooked via pre callback
 * @return true if the current function should be targeted.
 */
bool SL2Client::is_function_targeted(client_read_info *info) {
  Function function = info->function;
  const char *func_name = function_to_string(function);

  for (targetFunction t : parsedJson) {
    if (t.selected && STREQ(t.functionName.c_str(), func_name)) {
      if (t.mode & MATCH_INDEX && compare_indices(t, function)) {
        return true;
      }
      if (t.mode & MATCH_RETN_ADDRESS && compare_return_addresses(t, info)) {
        return true;
      }
      if (t.mode & MATCH_ARG_HASH && compare_arg_hashes(t, info)) {
        return true;
      }
      if (t.mode & MATCH_ARG_COMPARE && compare_arg_buffers(t, info)) {
        return true;
      }
      if (t.mode & MATCH_FILENAMES && compare_filenames(t, info)) {
        return true;
      }
      if (t.mode & MATCH_RETN_COUNT && compare_index_at_retaddr(t, info)) {
        return true;
      }
      if (t.mode & LOW_PRECISION) {
        // if filename is available
        if (info->source && compare_filenames(t, info)) {
          if (compare_filenames(t, info)) {
            return true;
          }
        }
        if (compare_return_addresses(t, info) && compare_arg_buffers(t, info)) {
          return true;
        }
      }
      if (t.mode & MEDIUM_PRECISION) {
        if (compare_arg_hashes(t, info) && compare_return_addresses(t, info)) {
          return true;
        }
      }
      if (t.mode & HIGH_PRECISION) {
        if (compare_arg_hashes(t, info) && compare_index_at_retaddr(t, info)) {
          return true;
        }
      }
    }
  }
  return false;
}

/**
 * compares the name of the file argument to this function with the name of the file argument
 * recorded by the wizard
 * @param t - struct describing the desired function to target, loaded from disk
 * @param info - information about the last hooked function call
 * @return true if they match, false if they don't
 */
bool SL2Client::compare_filenames(targetFunction &t, client_read_info *info) {
  return !wcscmp(t.source.c_str(), info->source);
}

/**
 * compares the number of times we've seen the given function
 * @param t - struct describing the desired function to target, loaded from disk
 * @param function - the function currently hooked
 * @return true if they match, false if they don't
 */
bool SL2Client::compare_indices(targetFunction &t, Function &function) {
  return call_counts[function] == t.index;
}

/**
 * compares the number of times we've encountered the given return address with the count from the
 * wizard
 * @param t - struct describing the desired function to target, loaded from disk
 * @param info - information about the last hooked function call
 * @return true if they match, false if they don't
 */
bool SL2Client::compare_index_at_retaddr(targetFunction &t, client_read_info *info) {
  return ret_addr_counts[info->retAddrOffset] == t.retAddrCount;
}

/**
 * compares the return address of the function with the one recorded by the wizard
 * @param t - struct describing the desired function to target, loaded from disk
 * @param info - information about the last hooked function call
 * @return true if they match, false if they don't
 */
bool SL2Client::compare_return_addresses(targetFunction &t, client_read_info *info) {
  // Get around ASLR by only examining the bottom bits. This is something of a cheap hack and we
  // should ideally store a copy of the memory map in every run
  uint64_t left = t.retAddrOffset & SUB_ASLR_BITS;
  uint64_t right = info->retAddrOffset & SUB_ASLR_BITS;
  // SL2_DR_DEBUG("Comparing 0x%llx to 0x%llx (%s)\n", left, right, left == right ? "True" :
  // "False");
  return left == right;
}

/**
 * compares the hash of the arguments for the function
 * @param t - struct describing the desired function to target, loaded from disk
 * @param info - information about the last hooked function call
 * @return true if they match, false if they don't
 */
bool SL2Client::compare_arg_hashes(targetFunction &t, client_read_info *info) {
  return STREQ(t.argHash.c_str(), info->argHash);
}

/**
 * compares the first 16 bytes of the argument buffer for a function vs the one recorded by the
 * wizard
 * @param t - struct describing the desired function to target, loaded from disk
 * @param info - information about the last hooked function call
 * @return true if they match, false if they don't
 */
bool SL2Client::compare_arg_buffers(targetFunction &t, client_read_info *info) { // Not working
  size_t minimum = min(16, t.buffer.size());
  if (info->lpNumberOfBytesRead) {
    minimum = min(minimum, *info->lpNumberOfBytesRead);
  } else {
    SL2_DR_DEBUG("[!] Couldn't get the size of the buffer! There's a small chance this could cause "
                 "a segfault\n");
  }

  return !memcmp(t.buffer.data(), info->lpBuffer, minimum);
}

/**
 * Increments the total number of call counts for this function
 * @param function
 * @return the incremented value
 */
uint64_t SL2Client::increment_call_count(Function function) {
  return call_counts[function]++;
}

/**
 * Increments the total number of return address counts for this return address
 * @param retAddr
 * @return the incremented value
 */
uint64_t SL2Client::increment_retaddr_count(uint64_t retAddr) {
  return ret_addr_counts[retAddr]++;
}

/**
 * Loads the msgpack blob containing target information into the client.
 * @param path - location of the target file to read from
 * @return succes
 */
bool SL2Client::loadTargets(string path) {
  file_t targets = dr_open_file(path.c_str(), DR_FILE_READ);
  size_t targets_size;
  size_t txsize;

  dr_file_size(targets, &targets_size);
  uint8_t *buffer = (uint8_t *)dr_global_alloc(targets_size);

  txsize = dr_read_file(targets, buffer, targets_size);
  dr_close_file(targets);

  if (txsize != targets_size) {
    dr_global_free(buffer, targets_size);
    return false;
  }

  std::vector<std::uint8_t> msg(buffer, buffer + targets_size);

  parsedJson = json::from_msgpack(msg);

  dr_global_free(buffer, targets_size);

  return parsedJson.is_array();
}

/*
    The next three functions are used to intercept __fastfail, which Windows
    provides to allow processes to request immediate termination.

    To get around this, we tell the target that __fastfail isn't avaiable.
    We then hope that they craft an exception record instead and send it
    to UnhandledException, where we intercept it and forward it to our
    exception handler. If the target decides to do neither of these, we
    still miss the exception.

    This trick was cribbed from WinAFL:
    https://github.com/ivanfratric/winafl/blob/73c7b41/winafl.c#L600

    NOTE(ww): These functions are duplicated across the fuzzer and the tracer.
    Keep them synced!
*/

/**
 * Hack to tell the target process that __fastfail isn't available
 */
void SL2Client::wrap_pre_IsProcessorFeaturePresent(void *wrapcxt, OUT void **user_data) {
#pragma warning(suppress : 4311 4302)
  DWORD feature = (DWORD)drwrap_get_arg(wrapcxt, 0);

#pragma warning(suppress : 4312)
  *user_data = (void *)feature;
}

/**
 * Hack to tell the target process that __fastfail isn't available
 */
void SL2Client::wrap_post_IsProcessorFeaturePresent(void *wrapcxt, void *user_data) {
#pragma warning(suppress : 4311 4302)
  DWORD feature = (DWORD)user_data;

  if (feature == PF_FASTFAIL_AVAILABLE) {
    SL2_DR_DEBUG(
        "wrap_post_IsProcessorFeaturePresent: got PF_FASTFAIL_AVAILABLE request, masking\n");
    drwrap_set_retval(wrapcxt, (void *)0);
  }
}

/**
 * Hack to tell the target process that __fastfail isn't available
 */
void SL2Client::wrap_pre_UnhandledExceptionFilter(void *wrapcxt, OUT void **user_data,
                                                  bool (*on_exception)(void *, dr_exception_t *)) {
  SL2_DR_DEBUG("wrap_pre_UnhandledExceptionFilter: stealing unhandled exception\n");

  EXCEPTION_POINTERS *exception = (EXCEPTION_POINTERS *)drwrap_get_arg(wrapcxt, 0);
  dr_exception_t excpt = {0};

  excpt.record = exception->ExceptionRecord;
  on_exception(drwrap_get_drcontext(wrapcxt), &excpt);
}

/**
    We also intercept VerifierStopMessage and VerifierStopMessageEx,
    which are supplied by Application Verifier for the purpose of catching
    heap corruptions.
 * @param on_exception function pointer to call with the wrap context
 */
void SL2Client::wrap_pre_VerifierStopMessage(void *wrapcxt, OUT void **user_data,
                                             bool (*on_exception)(void *, dr_exception_t *)) {
  SL2_DR_DEBUG("wrap_pre_VerifierStopMessage: stealing unhandled exception\n");

  EXCEPTION_RECORD record = {0};
  record.ExceptionCode = STATUS_HEAP_CORRUPTION;

  dr_exception_t excpt = {0};
  excpt.record = &record;

  on_exception(drwrap_get_drcontext(wrapcxt), &excpt);
}

/*
  The next several functions are wrappers that DynamoRIO calls before each of the targeted functions
  runs. Each of them records metadata about the target function call for use later.
*/

/** Pre-function wrapper for ReadEventLog

    bool ReadEventLog(
      _In_  HANDLE hEventLog,
      _In_  DWORD  dwReadFlags,
      _In_  DWORD  dwRecordOffset,
      _Out_ LPVOID lpBuffer,
      _In_  DWORD  nNumberOfBytesToRead,
      _Out_ DWORD  *pnBytesRead,
      _Out_ DWORD  *pnMinNumberOfBytesNeeded
    );

    Return: If the function succeeds, the return value is nonzero.
     * @param wrapcxt dynamorio wrap context
     * @param user_data pointer to a client_read_info struct to which we write information about
   this call
*/
void SL2Client::wrap_pre_ReadEventLog(void *wrapcxt, OUT void **user_data) {
  SL2_DR_DEBUG("<in wrap_pre_ReadEventLog>\n");
  HANDLE hEventLog = (HANDLE)drwrap_get_arg(wrapcxt, 0);
#pragma warning(suppress : 4311 4302)
  DWORD dwReadFlags = (DWORD)drwrap_get_arg(wrapcxt, 1);
#pragma warning(suppress : 4311 4302)
  DWORD dwRecordOffset = (DWORD)drwrap_get_arg(wrapcxt, 2);
  void *lpBuffer = (void *)drwrap_get_arg(wrapcxt, 3);
#pragma warning(suppress : 4311 4302)
  DWORD nNumberOfBytesToRead = (DWORD)drwrap_get_arg(wrapcxt, 4);
  DWORD *pnBytesRead = (DWORD *)drwrap_get_arg(wrapcxt, 5);
  DWORD *pnMinNumberOfBytesNeeded = (DWORD *)drwrap_get_arg(wrapcxt, 6);

  *user_data = dr_thread_alloc(drwrap_get_drcontext(wrapcxt), sizeof(client_read_info));
  client_read_info *info = (client_read_info *)*user_data;

  info->function = Function::ReadEventLog;
  info->hFile = hEventLog;
  info->lpBuffer = lpBuffer;
  info->nNumberOfBytesToRead = nNumberOfBytesToRead;
  info->lpNumberOfBytesRead = pnBytesRead;
  info->position = 0;
  info->retAddrOffset = (uint64_t)drwrap_get_retaddr(wrapcxt) - baseAddr;
  info->source = NULL;

  hash_context hash_ctx = {0};

  GetFinalPathNameByHandle(hEventLog, hash_ctx.fileName, MAX_PATH, FILE_NAME_NORMALIZED);
  hash_ctx.position = dwRecordOffset;
  hash_ctx.readSize = nNumberOfBytesToRead;

  info->argHash = (char *)dr_thread_alloc(drwrap_get_drcontext(wrapcxt), SL2_HASH_LEN + 1);
  hash_args(info->argHash, &hash_ctx);
}

/** Pre-function wrapper for RegQueryValu

    LONG WINAPI RegQueryValueEx(
      _In_        HKEY    hKey,
      _In_opt_    LPCTSTR lpValueName,
      _Reserved_  LPDWORD lpReserved,
      _Out_opt_   LPDWORD lpType,
      _Out_opt_   LPBYTE  lpData,
      _Inout_opt_ LPDWORD lpcbData
    );

    Return: If the function succeeds, the return value is ERROR_SUCCESS.
     * @param wrapcxt dynamorio wrap context
     * @param user_data pointer to a client_read_info struct to which we write information about
   this call
*/
void SL2Client::wrap_pre_RegQueryValueEx(void *wrapcxt, OUT void **user_data) {
  SL2_DR_DEBUG("<in wrap_pre_RegQueryValueEx>\n");
  HKEY hKey = (HKEY)drwrap_get_arg(wrapcxt, 0);
  LPCTSTR lpValueName = (LPCTSTR)drwrap_get_arg(wrapcxt, 1);
  LPDWORD lpReserved = (LPDWORD)drwrap_get_arg(wrapcxt, 2);
  LPDWORD lpType = (LPDWORD)drwrap_get_arg(wrapcxt, 3);
  LPBYTE lpData = (LPBYTE)drwrap_get_arg(wrapcxt, 4);
  LPDWORD lpcbData = (LPDWORD)drwrap_get_arg(wrapcxt, 5);

  if (lpData != NULL && lpcbData != NULL) {
    *user_data = dr_thread_alloc(drwrap_get_drcontext(wrapcxt), sizeof(client_read_info));
    client_read_info *info = (client_read_info *)*user_data;

    info->function = Function::RegQueryValueEx;
    info->hFile = hKey;
    info->lpBuffer = lpData;
    info->nNumberOfBytesToRead = *lpcbData;
    info->lpNumberOfBytesRead = lpcbData;
    info->position = 0;
    info->retAddrOffset = (uint64_t)drwrap_get_retaddr(wrapcxt) - baseAddr;
    info->source = NULL;

    hash_context hash_ctx = {0};

    //        mbstowcs_s(hash_ctx.fileName, , lpValueName, MAX_PATH);
    hash_ctx.readSize = *lpcbData;

    info->argHash = (char *)dr_thread_alloc(drwrap_get_drcontext(wrapcxt), SL2_HASH_LEN + 1);
    hash_args(info->argHash, &hash_ctx);
  } else {
    *user_data = NULL;
  }
}

/** Pre-function wrapper for WinHttpWebS

    DWORD WINAPI WinHttpWebSocketReceive(
      _In_  HINTERNET                      hWebSocket,
      _Out_ PVOID                          pvBuffer,
      _In_  DWORD                          dwBufferLength,
      _Out_ DWORD                          *pdwBytesRead,
      _Out_ WINHTTP_WEB_SOCKET_BUFFER_TYPE *peBufferType
    );

    Return: NO_ERROR on success. Otherwise an error code.
     * @param wrapcxt dynamorio wrap context
     * @param user_data pointer to a client_read_info struct to which we write information about
   this call
*/
void SL2Client::wrap_pre_WinHttpWebSocketReceive(void *wrapcxt, OUT void **user_data) {
  SL2_DR_DEBUG("<in wrap_pre_WinHttpWebSocketReceive>\n");
  HINTERNET hRequest = (HINTERNET)drwrap_get_arg(wrapcxt, 0);
  PVOID pvBuffer = drwrap_get_arg(wrapcxt, 1);
#pragma warning(suppress : 4311 4302)
  DWORD dwBufferLength = (DWORD)drwrap_get_arg(wrapcxt, 2);
  PDWORD pdwBytesRead = (PDWORD)drwrap_get_arg(wrapcxt, 3);
#pragma warning(suppress : 4311 4302)
  WINHTTP_WEB_SOCKET_BUFFER_TYPE peBufferType =
      (WINHTTP_WEB_SOCKET_BUFFER_TYPE)(int)drwrap_get_arg(wrapcxt, 3);

  // TODO: put this in another file cause you can't import wininet and winhttp
  // LONG positionHigh = 0;
  // DWORD positionLow = InternetSetFilePointer(hRequest, 0, &positionHigh, FILE_CURRENT);
  // uint64_t position = positionHigh;

  *user_data = dr_thread_alloc(drwrap_get_drcontext(wrapcxt), sizeof(client_read_info));
  client_read_info *info = (client_read_info *)*user_data;

  info->function = Function::WinHttpWebSocketReceive;
  info->hFile = hRequest;
  info->lpBuffer = pvBuffer;
  info->nNumberOfBytesToRead = dwBufferLength;
  info->lpNumberOfBytesRead = pdwBytesRead;
  info->position = 0;
  info->retAddrOffset = (uint64_t)drwrap_get_retaddr(wrapcxt) - baseAddr;
  info->source = NULL;

  hash_context hash_ctx = {0};

  //    hash_ctx.fileName[0] = (wchar_t) s;
  hash_ctx.readSize = dwBufferLength;

  info->argHash = (char *)dr_thread_alloc(drwrap_get_drcontext(wrapcxt), SL2_HASH_LEN + 1);
  hash_args(info->argHash, &hash_ctx);
}

/** Pre-function wrapper for InternetReadFile

    bool InternetReadFile(
      _In_  HINTERNET hFile,
      _Out_ LPVOID    lpBuffer,
      _In_  DWORD     dwNumberOfBytesToRead,
      _Out_ LPDWORD   lpdwNumberOfBytesRead
    );

    Return: Returns TRUE if successful
     * @param wrapcxt dynamorio wrap context
     * @param user_data pointer to a client_read_info struct to which we write information about
   this call
*/
void SL2Client::wrap_pre_InternetReadFile(void *wrapcxt, OUT void **user_data) {
  SL2_DR_DEBUG("<in wrap_pre_InternetReadFile>\n");
  HINTERNET hFile = (HINTERNET)drwrap_get_arg(wrapcxt, 0);
  void *lpBuffer = drwrap_get_arg(wrapcxt, 1);
#pragma warning(suppress : 4311 4302)
  DWORD nNumberOfBytesToRead = (DWORD)drwrap_get_arg(wrapcxt, 2);
  LPDWORD lpNumberOfBytesRead = (LPDWORD)drwrap_get_arg(wrapcxt, 3);

  // LONG positionHigh = 0;
  // DWORD positionLow = InternetSetFilePointer(hFile, 0, &positionHigh, FILE_CURRENT);
  // uint64_t position = positionHigh;

  *user_data = dr_thread_alloc(drwrap_get_drcontext(wrapcxt), sizeof(client_read_info));
  client_read_info *info = (client_read_info *)*user_data;

  info->function = Function::InternetReadFile;
  info->hFile = hFile;
  info->lpBuffer = lpBuffer;
  info->nNumberOfBytesToRead = nNumberOfBytesToRead;
  info->lpNumberOfBytesRead = lpNumberOfBytesRead;
  info->position = 0;
  info->retAddrOffset = (uint64_t)drwrap_get_retaddr(wrapcxt) - baseAddr;
  info->source = NULL;

  hash_context hash_ctx = {0};

  //    hash_ctx.fileName[0] = (wchar_t) s;
  hash_ctx.readSize = nNumberOfBytesToRead;

  info->argHash = (char *)dr_thread_alloc(drwrap_get_drcontext(wrapcxt), SL2_HASH_LEN + 1);
  hash_args(info->argHash, &hash_ctx);
}

/** Pre-function wrapper for WinHttpReadD

    bool WINAPI WinHttpReadData(
      _In_  HINTERNET hRequest,
      _Out_ LPVOID    lpBuffer,
      _In_  DWORD     dwNumberOfBytesToRead,
      _Out_ LPDWORD   lpdwNumberOfBytesRead
    );

    Return: Returns TRUE if successful
     * @param wrapcxt dynamorio wrap context
     * @param user_data pointer to a client_read_info struct to which we write information about
   this call
*/
void SL2Client::wrap_pre_WinHttpReadData(void *wrapcxt, OUT void **user_data) {
  SL2_DR_DEBUG("<in wrap_pre_WinHttpReadData>\n");
  HINTERNET hRequest = (HINTERNET)drwrap_get_arg(wrapcxt, 0);
  void *lpBuffer = drwrap_get_arg(wrapcxt, 1);
#pragma warning(suppress : 4311 4302)
  DWORD nNumberOfBytesToRead = (DWORD)drwrap_get_arg(wrapcxt, 2);
  DWORD *lpNumberOfBytesRead = (DWORD *)drwrap_get_arg(wrapcxt, 3);

  // LONG positionHigh = 0;
  // DWORD positionLow = InternetSetFilePointer(hRequest, 0, &positionHigh, FILE_CURRENT);
  // uint64_t position = positionHigh;

  *user_data = dr_thread_alloc(drwrap_get_drcontext(wrapcxt), sizeof(client_read_info));
  client_read_info *info = (client_read_info *)*user_data;

  info->function = Function::WinHttpReadData;
  info->hFile = hRequest;
  info->lpBuffer = lpBuffer;
  info->nNumberOfBytesToRead = nNumberOfBytesToRead;
  info->lpNumberOfBytesRead = lpNumberOfBytesRead;
  info->position = 0;
  info->retAddrOffset = (uint64_t)drwrap_get_retaddr(wrapcxt) - baseAddr;
  info->source = NULL;

  hash_context hash_ctx = {0};

  //    hash_ctx.fileName[0] = (wchar_t) s;
  hash_ctx.readSize = nNumberOfBytesToRead;

  info->argHash = (char *)dr_thread_alloc(drwrap_get_drcontext(wrapcxt), SL2_HASH_LEN + 1);
  hash_args(info->argHash, &hash_ctx);
}

/** Pre-function wrapper for recv

    int recv(
      _In_  SOCKET s,
      _Out_ char   *buf,
      _In_  int    len,
      _In_  int    flags
    );

    Return: recv returns the number of bytes received

     * @param user_data pointer to a client_read_info struct to which we write information about
   this call
*/
void SL2Client::wrap_pre_recv(void *wrapcxt, OUT void **user_data) {
  SL2_DR_DEBUG("<in wrap_pre_recv>\n");
  SOCKET s = (SOCKET)drwrap_get_arg(wrapcxt, 0);
  char *buf = (char *)drwrap_get_arg(wrapcxt, 1);
#pragma warning(suppress : 4311 4302)
  int len = (int)drwrap_get_arg(wrapcxt, 2);
#pragma warning(suppress : 4311 4302)
  int flags = (int)drwrap_get_arg(wrapcxt, 3);

  *user_data = dr_thread_alloc(drwrap_get_drcontext(wrapcxt), sizeof(client_read_info));
  client_read_info *info = (client_read_info *)*user_data;

  info->function = Function::recv;
  info->hFile = NULL;
  info->lpBuffer = buf;
  info->nNumberOfBytesToRead = len;
  info->lpNumberOfBytesRead = NULL;
  info->position = 0;
  info->retAddrOffset = (uint64_t)drwrap_get_retaddr(wrapcxt) - baseAddr;
  info->source = NULL;

  hash_context hash_ctx = {0};

  hash_ctx.fileName[0] = (wchar_t)s;
  hash_ctx.readSize = len;

  info->argHash = (char *)dr_thread_alloc(drwrap_get_drcontext(wrapcxt), SL2_HASH_LEN + 1);
  hash_args(info->argHash, &hash_ctx);
}

/** Pre-function wrapper for ReadFile

    bool WINAPI ReadFile(
      _In_        HANDLE       hFile,
      _Out_       LPVOID       lpBuffer,
      _In_        DWORD        nNumberOfBytesToRead,
      _Out_opt_   LPDWORD      lpNumberOfBytesRead,
      _Inout_opt_ LPOVERLAPPED lpOverlapped
    );

    Return: If the function succeeds, the return value is nonzero (TRUE).

 * @param wrapcxt the DynamoRIO Context for the wrapped function
 * @param user_data pointer to a client_read_info struct to which we write information about this
 call
*/
void SL2Client::wrap_pre_ReadFile(void *wrapcxt, OUT void **user_data) {
  SL2_DR_DEBUG("<in wrap_pre_ReadFile>\n");
  HANDLE hFile = drwrap_get_arg(wrapcxt, 0);
  void *lpBuffer = drwrap_get_arg(wrapcxt, 1);
#pragma warning(suppress : 4311 4302)
  DWORD nNumberOfBytesToRead = (DWORD)drwrap_get_arg(wrapcxt, 2);
  DWORD *lpNumberOfBytesRead = (DWORD *)drwrap_get_arg(wrapcxt, 3);

  hash_context hash_ctx = {0};

  LARGE_INTEGER offset = {0};
  LARGE_INTEGER position = {0};
  SetFilePointerEx(hFile, offset, &position, FILE_CURRENT);

  GetFinalPathNameByHandle(hFile, hash_ctx.fileName, MAX_PATH, FILE_NAME_NORMALIZED);
  hash_ctx.position = position.QuadPart;
  hash_ctx.readSize = nNumberOfBytesToRead;

  *user_data = dr_thread_alloc(drwrap_get_drcontext(wrapcxt), sizeof(client_read_info));
  client_read_info *info = (client_read_info *)*user_data;

  info->function = Function::ReadFile;
  info->hFile = hFile;
  info->lpBuffer = lpBuffer;
  info->nNumberOfBytesToRead = nNumberOfBytesToRead;
  info->lpNumberOfBytesRead = lpNumberOfBytesRead;
  info->position = hash_ctx.position;
  info->retAddrOffset = (uint64_t)drwrap_get_retaddr(wrapcxt) - baseAddr;

  info->source =
      (wchar_t *)dr_thread_alloc(drwrap_get_drcontext(wrapcxt), sizeof(hash_ctx.fileName));
  memcpy(info->source, hash_ctx.fileName, sizeof(hash_ctx.fileName));

  info->argHash = (char *)dr_thread_alloc(drwrap_get_drcontext(wrapcxt), SL2_HASH_LEN + 1);
  hash_args(info->argHash, &hash_ctx);
}

/**
 * Pre-function wrapper for fread_s
 * @param wrapcxt - dynamorio wrap context
 * @param user_data - pointer to client_read_info struct to populate with information about the
 * function call
 */
void SL2Client::wrap_pre_fread_s(void *wrapcxt, OUT void **user_data) {
  SL2_DR_DEBUG("<in wrap_pre_fread_s>\n");
  void *buffer = (void *)drwrap_get_arg(wrapcxt, 0);
  size_t bufsize = (size_t)drwrap_get_arg(wrapcxt, 1);
#pragma warning(suppress : 4311 4302)
  size_t size = (size_t)drwrap_get_arg(wrapcxt, 2);
#pragma warning(suppress : 4311 4302)
  size_t count = (size_t)drwrap_get_arg(wrapcxt, 3);
  FILE *file = (FILE *)drwrap_get_arg(wrapcxt, 4);

  *user_data = dr_thread_alloc(drwrap_get_drcontext(wrapcxt), sizeof(client_read_info));
  client_read_info *info = (client_read_info *)*user_data;

  info->function = Function::fread_s;
  // TODO(ww): Figure out why _get_osfhandle breaks DR.
  // info->hFile             = (HANDLE) _get_osfhandle(_fileno(file));
  info->hFile = NULL;
  info->lpBuffer = buffer;
  info->nNumberOfBytesToRead = size * count;
  info->lpNumberOfBytesRead = NULL;
  info->position = 0;
  info->retAddrOffset = (uint64_t)drwrap_get_retaddr(wrapcxt) - baseAddr;
  info->source = NULL;

  hash_context hash_ctx = {0};

  hash_ctx.fileName[0] = (wchar_t)_fileno(file);

  hash_ctx.position = bufsize; // Field names don't actually matter
  hash_ctx.readSize = size;
  hash_ctx.count = count;

  info->argHash = (char *)dr_thread_alloc(drwrap_get_drcontext(wrapcxt), SL2_HASH_LEN + 1);
  hash_args(info->argHash, &hash_ctx);
}

/**
 * Pre-function wrapper for fread
 * @param wrapcxt - dynamorio wrap context
 * @param user_data - pointer to client_read_info struct to populate with information about the
 * function call
 */
void SL2Client::wrap_pre_fread(void *wrapcxt, OUT void **user_data) {
  SL2_DR_DEBUG("<in wrap_pre_fread>\n");

  void *buffer = (void *)drwrap_get_arg(wrapcxt, 0);
#pragma warning(suppress : 4311 4302)
  size_t size = (size_t)drwrap_get_arg(wrapcxt, 1);
#pragma warning(suppress : 4311 4302)
  size_t count = (size_t)drwrap_get_arg(wrapcxt, 2);
  FILE *file = (FILE *)drwrap_get_arg(wrapcxt, 3);

  *user_data = dr_thread_alloc(drwrap_get_drcontext(wrapcxt), sizeof(client_read_info));
  client_read_info *info = (client_read_info *)*user_data;

  info->function = Function::fread;
  // TODO(ww): Figure out why _get_osfhandle breaks DR.
  // info->hFile             = (HANDLE) _get_osfhandle(_fileno(file));
  info->hFile = NULL;
  info->lpBuffer = buffer;
  info->nNumberOfBytesToRead = size * count;
  info->lpNumberOfBytesRead = NULL;
  info->position = 0;
  info->retAddrOffset = (uint64_t)drwrap_get_retaddr(wrapcxt) - baseAddr;
  info->source = NULL;

  hash_context hash_ctx = {0};

  hash_ctx.fileName[0] = (wchar_t)_fileno(file);

  //    hash_ctx.position = ftell(fpointer);  // This instantly crashes DynamoRIO
  hash_ctx.readSize = size;
  hash_ctx.count = count;

  info->argHash = (char *)dr_thread_alloc(drwrap_get_drcontext(wrapcxt), SL2_HASH_LEN + 1);
  hash_args(info->argHash, &hash_ctx);
}

/**
 * Pre-function wrapper for _read
 * @param wrapcxt - dynamorio wrap context
 * @param user_data - pointer to client_read_info struct to populate with information about the
 * function call
 */
void SL2Client::wrap_pre__read(void *wrapcxt, OUT void **user_data) {
  SL2_DR_DEBUG("<in wrap_pre__read>\n");

#pragma warning(suppress : 4311 4302)
  int fd = (int)drwrap_get_arg(wrapcxt, 0);
  void *buffer = drwrap_get_arg(wrapcxt, 1);
#pragma warning(suppress : 4311 4302)
  unsigned int count = (unsigned int)drwrap_get_arg(wrapcxt, 2);

  *user_data = dr_thread_alloc(drwrap_get_drcontext(wrapcxt), sizeof(client_read_info));
  client_read_info *info = (client_read_info *)*user_data;

  info->function = Function::_read;
  // TODO(ww): Figure out why _get_osfhandle breaks DR.
  // info->hFile             = (HANDLE) _get_osfhandle(fd);
  info->hFile = NULL;
  info->lpBuffer = buffer;
  info->nNumberOfBytesToRead = count;
  info->lpNumberOfBytesRead = NULL;
  info->position = 0;
  info->retAddrOffset = (uint64_t)drwrap_get_retaddr(wrapcxt) - baseAddr;
  info->source = NULL;

  hash_context hash_ctx = {0};

  hash_ctx.fileName[0] = (wchar_t)fd;
  hash_ctx.count = count;

  info->argHash = (char *)dr_thread_alloc(drwrap_get_drcontext(wrapcxt), SL2_HASH_LEN + 1);
  hash_args(info->argHash, &hash_ctx);
}

/**
 * Pre-function wrapper for MapViewOfFile. Hsa to rewrite the access arguments to FILE_MAP_COPY
 * @param wrapcxt - dynamorio wrap context
 * @param user_data - pointer to client_read_info struct to populate with information about the
 * function call
 */
void SL2Client::wrap_pre_MapViewOfFile(void *wrapcxt, OUT void **user_data) {
  SL2_DR_DEBUG("<in wrap_pre_MapViewOfFile>\n");

  HANDLE hFileMappingObject = drwrap_get_arg(wrapcxt, 0);
#pragma warning(suppress : 4311 4302)
  DWORD dwDesiredAccess = (DWORD)drwrap_get_arg(wrapcxt, 1);
#pragma warning(suppress : 4311 4302)
  DWORD dwFileOffsetHigh = (DWORD)drwrap_get_arg(wrapcxt, 2);
#pragma warning(suppress : 4311 4302)
  DWORD dwFileOffsetLow = (DWORD)drwrap_get_arg(wrapcxt, 3);
  size_t dwNumberOfBytesToMap = (size_t)drwrap_get_arg(wrapcxt, 4);

  *user_data = dr_thread_alloc(drwrap_get_drcontext(wrapcxt), sizeof(client_read_info));
  client_read_info *info = (client_read_info *)*user_data;

  info->function = Function::MapViewOfFile;
  info->hFile = hFileMappingObject;
  // NOTE(ww): dwNumberOfBytesToMap=0 is a special case here, since 0 indicates that the
  // entire file is being mapped into memory. We handle this case in the post-hook
  // with a VirtualQuery call.
  info->nNumberOfBytesToRead = dwNumberOfBytesToMap;
  info->position = 0;
  info->retAddrOffset = (uint64_t)drwrap_get_retaddr(wrapcxt) - baseAddr;
  info->source = NULL;

  // NOTE(ww): We populate these in the post-hook, when necessary.
  info->lpBuffer = NULL;
  info->argHash = (char *)dr_thread_alloc(drwrap_get_drcontext(wrapcxt), SL2_HASH_LEN + 1);

  // Change write-access requests to copy-on-write requests, since we don't want to clobber
  // our original input file with mutated data.
  // TODO(ww): Is this going to cause problems for programs that attept to create multiple
  // different memory maps of the same on-disk file?
  if (dwDesiredAccess & FILE_MAP_ALL_ACCESS || dwDesiredAccess & FILE_MAP_WRITE) {
    SL2_DR_DEBUG("user requested write access from MapViewOfFile, changing to CoW!\n");
    uint64_t fixed_access = FILE_MAP_COPY;

    fixed_access |= (dwDesiredAccess & FILE_MAP_EXECUTE);

    drwrap_set_arg(wrapcxt, 1, (void *)fixed_access);
  }
}

/**
 * Sanity check to make sure we don't get null pointers in the post hook. Important for file mapping
 * since we rewrite the arguments
 * @param wrapcxt the dynamorio wrap context
 * @param user_data pointer to the function call info struct
 * @param drcontext the dynamorio execution context
 * @return whether everything looks okay
 */
bool SL2Client::is_sane_post_hook(void *wrapcxt, void *user_data, void **drcontext) {
  if (!user_data) {
    SL2_DR_DEBUG("Fatal: user_data=NULL in post-hook!\n");
    return false;
  }

  if (!wrapcxt) {
    SL2_DR_DEBUG("Warning: wrapcxt=NULL in post-hook, using dr_get_current_drcontext!\n");
    *drcontext = dr_get_current_drcontext();
  } else {
    *drcontext = drwrap_get_drcontext(wrapcxt);
  }

  return true;
}

/**
 * Simple mapping from functions to their stringified names
 * @param function member of the Function enum
 * @return a string with the name of the function
 */
const char *SL2Client::function_to_string(Function function) {
  switch (function) {
  case Function::ReadFile:
    return "ReadFile";
  case Function::recv:
    return "recv";
  case Function::WinHttpReadData:
    return "WinHttpReadData";
  case Function::InternetReadFile:
    return "InternetReadFile";
  case Function::WinHttpWebSocketReceive:
    return "WinHttpWebSocketReceive";
  case Function::RegQueryValueEx:
    return "RegQueryValueEx";
  case Function::ReadEventLog:
    return "ReadEventLog";
  case Function::fread:
    return "fread";
  case Function::fread_s:
    return "fread_s";
  case Function::_read:
    return "_read";
  case Function::MapViewOfFile:
    return "MapViewOfFile";
  }

  return "unknown";
}

/**
 * maps exception codes to strings
 * @param exception_code
 * @return - stringifed version of the exception code
 */
const char *SL2Client::exception_to_string(DWORD exception_code) {
  char *exception_str;

  switch (exception_code) {
  case EXCEPTION_ACCESS_VIOLATION:
    exception_str = "EXCEPTION_ACCESS_VIOLATION";
    break;
  case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    exception_str = "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
    break;
  case EXCEPTION_BREAKPOINT:
    exception_str = "EXCEPTION_BREAKPOINT";
    break;
  case EXCEPTION_DATATYPE_MISALIGNMENT:
    exception_str = "EXCEPTION_DATATYPE_MISALIGNMENT";
    break;
  case EXCEPTION_FLT_DENORMAL_OPERAND:
    exception_str = "EXCEPTION_FLT_DENORMAL_OPERAND";
    break;
  case EXCEPTION_FLT_DIVIDE_BY_ZERO:
    exception_str = "EXCEPTION_FLT_DIVIDE_BY_ZERO";
    break;
  case EXCEPTION_FLT_INEXACT_RESULT:
    exception_str = "EXCEPTION_FLT_INEXACT_RESULT";
    break;
  case EXCEPTION_FLT_INVALID_OPERATION:
    exception_str = "EXCEPTION_FLT_INVALID_OPERATION";
    break;
  case EXCEPTION_FLT_OVERFLOW:
    exception_str = "EXCEPTION_FLT_OVERFLOW";
    break;
  case EXCEPTION_FLT_STACK_CHECK:
    exception_str = "EXCEPTION_FLT_STACK_CHECK";
    break;
  case EXCEPTION_FLT_UNDERFLOW:
    exception_str = "EXCEPTION_FLT_UNDERFLOW";
    break;
  case EXCEPTION_ILLEGAL_INSTRUCTION:
    exception_str = "EXCEPTION_ILLEGAL_INSTRUCTION";
    break;
  case EXCEPTION_IN_PAGE_ERROR:
    exception_str = "EXCEPTION_IN_PAGE_ERROR";
    break;
  case EXCEPTION_INT_DIVIDE_BY_ZERO:
    exception_str = "EXCEPTION_INT_DIVIDE_BY_ZERO";
    break;
  case EXCEPTION_INT_OVERFLOW:
    exception_str = "EXCEPTION_INT_OVERFLOW";
    break;
  case EXCEPTION_INVALID_DISPOSITION:
    exception_str = "EXCEPTION_INVALID_DISPOSITION";
    break;
  case EXCEPTION_NONCONTINUABLE_EXCEPTION:
    exception_str = "EXCEPTION_NONCONTINUABLE_EXCEPTION";
    break;
  case EXCEPTION_PRIV_INSTRUCTION:
    exception_str = "EXCEPTION_PRIV_INSTRUCTION";
    break;
  case EXCEPTION_SINGLE_STEP:
    exception_str = "EXCEPTION_SINGLE_STEP";
    break;
  case EXCEPTION_STACK_OVERFLOW:
    exception_str = "EXCEPTION_STACK_OVERFLOW";
    break;
  case STATUS_HEAP_CORRUPTION:
    exception_str = "STATUS_HEAP_CORRUPTION";
    break;
  default:
    exception_str = "EXCEPTION_SL2_UNKNOWN";
    break;
  }

  return exception_str;
}

/**
 * iterates over SL2_FUNCMOD_TABLE and checks whether the function exists somewhere
 * @param func - function to check
 * @param mod - mane of the module to check against
 * @return whether we found a corresponding mapping from function -> module
 */
bool SL2Client::function_is_in_expected_module(const char *func, const char *mod) {
  for (int i = 0; i < SL2_FUNCMOD_TABLE_SIZE; i++) {
    if (STREQ(func, SL2_FUNCMOD_TABLE[i].func) && STREQI(mod, SL2_FUNCMOD_TABLE[i].mod)) {
      return true;
    }
  }

  return false;
}

// TODO(ww): Document the fallback values here.
/**
 * Converts a msgpack-encoded target object on the disk into a target function struct
 *
 * FALLBACK VALUES (If one of the keys is missing):

    selected --  false

    callCount --  -1

    retAddrCount --  -1

    mode --  MATCH_INDEX

    retAddrOffset --  -1

    func_name --  ""

    argHash --  ""
 * @param j - values loaded from target file on disk
 * @param t - struct to fill out
 */
SL2_EXPORT
void from_json(const json &j, targetFunction &t) {
  t.selected = j.value("selected", false);
  t.index = j.value("callCount", -1);
  t.retAddrCount = j.value("retAddrCount", -1);
  t.mode = j.value("mode", MATCH_INDEX); // TODO - might want to chose a more sensible default
  t.retAddrOffset = j.value("retAddrOffset", -1);
  t.functionName = j.value("func_name", "");
  t.argHash = j.value("argHash", "");
  t.buffer = j["buffer"].get<vector<uint8_t>>();

  string source = j.value("source", "");
  wstring wsource;
  wsource.assign(source.begin(), source.end());
  t.source = wsource;
}

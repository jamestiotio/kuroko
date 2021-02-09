/**
 * Currently just uname().
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#ifndef _WIN32
#include <sys/utsname.h>
#else
#include <windows.h>
#endif

#include "vm.h"
#include "value.h"
#include "object.h"
#include "util.h"

/* Did you know this is actually specified to not exist in a header? */
extern char ** environ;

KrkClass * OSError = NULL;

#ifndef _WIN32
KRK_FUNC(uname,{
	struct utsname buf;
	if (uname(&buf) < 0) return NONE_VAL();

	KRK_PAUSE_GC();

	KrkValue result = krk_dict_of(5 * 2, (KrkValue[]) {
		OBJECT_VAL(S("sysname")), OBJECT_VAL(krk_copyString(buf.sysname,strlen(buf.sysname))),
		OBJECT_VAL(S("nodename")), OBJECT_VAL(krk_copyString(buf.nodename,strlen(buf.nodename))),
		OBJECT_VAL(S("release")), OBJECT_VAL(krk_copyString(buf.release,strlen(buf.release))),
		OBJECT_VAL(S("version")), OBJECT_VAL(krk_copyString(buf.version,strlen(buf.version))),
		OBJECT_VAL(S("machine")), OBJECT_VAL(krk_copyString(buf.machine,strlen(buf.machine)))
	});

	KRK_RESUME_GC();
	return result;
})
#else
KRK_FUNC(uname,{
	KRK_PAUSE_GC();

	TCHAR buffer[256] = TEXT("");
	DWORD dwSize = sizeof(buffer);
	GetComputerName(buffer, &dwSize);

	OSVERSIONINFOA versionInfo = {0};
	versionInfo.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);
	GetVersionExA(&versionInfo);
	KrkValue release;
	if (versionInfo.dwMajorVersion == 10) {
		release = OBJECT_VAL(S("10"));
	} else if (versionInfo.dwMajorVersion == 6) {
		if (versionInfo.dwMinorVersion == 3) {
			release = OBJECT_VAL(S("8.1"));
		} else if (versionInfo.dwMinorVersion == 2) {
			release = OBJECT_VAL(S("8.0"));
		} else if (versionInfo.dwMinorVersion == 1) {
			release = OBJECT_VAL(S("7"));
		} else if (versionInfo.dwMinorVersion == 0) {
			release = OBJECT_VAL(S("Vista"));
		}
	} else {
		release = OBJECT_VAL(S("XP or earlier"));
	}

	char tmp[256];
	sprintf(tmp, "%ld", versionInfo.dwBuildNumber);

	KrkValue version = OBJECT_VAL(krk_copyString(tmp,strlen(tmp)));
	KrkValue machine;
	if (sizeof(void *) == 8) {
		machine = OBJECT_VAL(S("x64"));
	} else {
		machine = OBJECT_VAL(S("x86"));
	}

	KrkValue result = krk_dict_of(5 * 2, (KrkValue[]) {
		OBJECT_VAL(S("sysname")), OBJECT_VAL(S("Windows")),
		OBJECT_VAL(S("nodename")), OBJECT_VAL(krk_copyString(buffer,dwSize)),
		OBJECT_VAL(S("release")), release,
		OBJECT_VAL(S("version")), version,
		OBJECT_VAL(S("machine")), machine
	});

	KRK_RESUME_GC();
	return result;
})
#endif

KrkClass * environClass;

KrkValue krk_os_setenviron(int argc, KrkValue argv[]) {
	if (argc < 3 || !krk_isInstanceOf(argv[0], environClass) ||
		!IS_STRING(argv[1]) || !IS_STRING(argv[2])) {
		return krk_runtimeError(vm.exceptions.argumentError, "Invalid arguments to environ.__set__");
	}
	/* Set environment variable */
	char * tmp = malloc(AS_STRING(argv[1])->length + AS_STRING(argv[2])->length + 2);
	sprintf(tmp, "%s=%s", AS_CSTRING(argv[1]), AS_CSTRING(argv[2]));
	int r = putenv(tmp);
	if (r == 0) {
		/* Make super call */
		krk_push(argv[0]);
		krk_push(argv[1]);
		krk_push(argv[2]);
		return krk_callSimple(OBJECT_VAL(vm.baseClasses.dictClass->_setter), 3, 0);
	} else {
		/* OSError? */
		return krk_runtimeError(vm.exceptions.baseException, strerror(errno));
	}
}

KrkValue krk_os_unsetenviron(int argc, KrkValue argv[]) {
	if (argc < 2 || !krk_isInstanceOf(argv[0], environClass) ||
		!IS_STRING(argv[1])) {
		return krk_runtimeError(vm.exceptions.argumentError, "Invalid arguments to environ.__delitem__");
	}
#ifndef _WIN32
	unsetenv(AS_CSTRING(argv[1]));
#else
	char * tmp = malloc(AS_STRING(argv[1])->length + 2);
	sprintf(tmp, "%s=", AS_CSTRING(argv[1]));
	putenv(tmp);
	free(tmp);
#endif
	krk_push(argv[0]);
	krk_push(argv[1]);
	return krk_callSimple(OBJECT_VAL(vm.baseClasses.dictClass->_delitem), 2, 0);
}

static void _loadEnviron(KrkInstance * module) {
	/* Create a new class to subclass `dict` */
	KrkString * className = S("_Environ");
	krk_push(OBJECT_VAL(className));
	environClass = krk_newClass(className, vm.baseClasses.dictClass);
	krk_attachNamedObject(&module->fields, "_Environ", (KrkObj*)environClass);
	krk_pop(); /* className */

	/* Add our set method that should also call dict's set method */
	krk_defineNative(&environClass->methods, ".__set__", krk_os_setenviron);
	krk_defineNative(&environClass->methods, ".__delitem__", krk_os_unsetenviron);
	krk_finalizeClass(environClass);

	/* Start with an empty dictionary */
	KrkInstance * environObj = AS_INSTANCE(krk_dict_of(0,NULL));
	krk_push(OBJECT_VAL(environObj));

	/* Transform it into an _Environ */
	environObj->_class = environClass;

	/* And attach it to the module */
	krk_attachNamedObject(&module->fields, "environ", (KrkObj*)environObj);
	krk_pop();

	/* Now load the environment into it */
	if (!environ) return; /* Empty environment */

	char ** env = environ;
	for (; *env; env++) {
		const char * equals = strchr(*env, '=');
		if (!equals) continue;

		size_t len = strlen(*env);
		size_t keyLen = equals - *env;
		size_t valLen = len - keyLen - 1;

		KrkValue key = OBJECT_VAL(krk_copyString(*env, keyLen));
		krk_push(key);
		KrkValue val = OBJECT_VAL(krk_copyString(equals+1, valLen));
		krk_push(val);

		krk_tableSet(AS_DICT(OBJECT_VAL(environObj)), key, val);
		krk_pop(); /* val */
		krk_pop(); /* key */
	}

}

KRK_FUNC(system,{
	FUNCTION_TAKES_EXACTLY(1);
	CHECK_ARG(0,str,KrkString*,cmd);
	return INTEGER_VAL(system(cmd->chars));
})

KRK_FUNC(getcwd,{
	FUNCTION_TAKES_NONE();
	char buf[4096]; /* TODO PATH_MAX? */
	if (!getcwd(buf, 4096)) return krk_runtimeError(OSError, strerror(errno));
	return OBJECT_VAL(krk_copyString(buf, strlen(buf)));
})

KRK_FUNC(chdir,{
	FUNCTION_TAKES_EXACTLY(1);
	CHECK_ARG(0,str,KrkString*,newDir);
	if (chdir(newDir->chars)) return krk_runtimeError(OSError, strerror(errno));
})

KRK_FUNC(getpid,{
	FUNCTION_TAKES_NONE();
	return INTEGER_VAL(getpid());
})

KRK_FUNC(strerror,{
	FUNCTION_TAKES_EXACTLY(1);
	CHECK_ARG(0,int,krk_integer_type,errorNo);
	char *s = strerror(errorNo);
	if (!s) return NONE_VAL();
	return OBJECT_VAL(krk_copyString(s,strlen(s)));
})

KRK_FUNC(access,{
	FUNCTION_TAKES_EXACTLY(2);
	CHECK_ARG(0,str,KrkString*,path);
	CHECK_ARG(1,int,krk_integer_type,mask);
	if (access(path->chars, mask) == 0) return BOOLEAN_VAL(1);
	return BOOLEAN_VAL(0);
})

#ifndef _WIN32
KRK_FUNC(kill,{
	FUNCTION_TAKES_EXACTLY(2);
	return INTEGER_VAL(kill(AS_INTEGER(argv[0]), AS_INTEGER(argv[1])));
})

KRK_FUNC(fork,{
	FUNCTION_TAKES_NONE();
	return INTEGER_VAL(fork());
})
#endif

KrkValue krk_module_onload_os(void) {
	KrkInstance * module = krk_newInstance(vm.moduleClass);
	/* Store it on the stack for now so we can do stuff that may trip GC
	 * and not lose it to garbage colletion... */
	krk_push(OBJECT_VAL(module));

#ifdef _WIN32
	krk_attachNamedObject(&module->fields, "name", (KrkObj*)S("nt"));
#else
	krk_attachNamedObject(&module->fields, "name", (KrkObj*)S("posix"));
#endif

	OSError = krk_newClass(S("OSError"), vm.exceptions.baseException);
	krk_attachNamedObject(&module->fields, "OSError", (KrkObj*)OSError);
	krk_finalizeClass(OSError);

	BIND_FUNC(module,uname);
	BIND_FUNC(module,system);
	BIND_FUNC(module,getcwd);
	BIND_FUNC(module,chdir);
	BIND_FUNC(module,getpid);
	BIND_FUNC(module,strerror);
#ifndef _WIN32
	BIND_FUNC(module,kill);
	BIND_FUNC(module,fork);
#endif

	krk_attachNamedValue(&module->fields, "F_OK", INTEGER_VAL(F_OK));
	krk_attachNamedValue(&module->fields, "R_OK", INTEGER_VAL(R_OK));
	krk_attachNamedValue(&module->fields, "W_OK", INTEGER_VAL(W_OK));
	krk_attachNamedValue(&module->fields, "X_OK", INTEGER_VAL(X_OK));
	BIND_FUNC(module,access);

	_loadEnviron(module);

	/* Pop the module object before returning; it'll get pushed again
	 * by the VM before the GC has a chance to run, so it's safe. */
	assert(AS_INSTANCE(krk_pop()) == module);
	return OBJECT_VAL(module);
}



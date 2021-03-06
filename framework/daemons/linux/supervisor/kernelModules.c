//--------------------------------------------------------------------------------------------------
/** @file kernelModules.c
 *
 * API for managing Legato-bundled kernel modules.
 *
 * Copyright (C) Sierra Wireless Inc.
 */
#include "legato.h"
#include "limit.h"
#include "fileDescriptor.h"
#include "smack.h"
#include "sysPaths.h"
#include "kernelModules.h"
#include "le_cfg_interface.h"


//--------------------------------------------------------------------------------------------------
/**
 * Memory pool size for module objects and strings
 */
//--------------------------------------------------------------------------------------------------
#define KMODULE_DEFAULT_POOL_SIZE 8
#define STRINGS_DEFAULT_POOL_SIZE 8


//--------------------------------------------------------------------------------------------------
/**
 * Maximum number of parameters passed to a kernel module during insmod.
 */
//--------------------------------------------------------------------------------------------------
#define KMODULE_MAX_ARGC 256


//--------------------------------------------------------------------------------------------------
/**
 * Maximum parameter string buffer size in the form of "<name>=<value>\0".
 * Use maximum name and string value size from configTree.
 * Allow extra space (2 bytes) for enclosing value in quotes, if necessary.
 */
//--------------------------------------------------------------------------------------------------
#define STRINGS_MAX_BUFFER_SIZE (LE_CFG_NAME_LEN + LE_CFG_STR_LEN + 2 + 2)


//--------------------------------------------------------------------------------------------------
/**
 * Root of configTree containing module parameters
 */
//--------------------------------------------------------------------------------------------------
#define KMODULE_CONFIG_TREE_ROOT "/modules"


//--------------------------------------------------------------------------------------------------
/**
 * Module insert command and format, arguments are module path and module params
 */
//--------------------------------------------------------------------------------------------------
#define INSMOD_COMMAND "/sbin/insmod"
#define INSMOD_COMMAND_FORMAT INSMOD_COMMAND" %s %s"


//--------------------------------------------------------------------------------------------------
/**
 * Module remove command and format, argument is module name
 */
//--------------------------------------------------------------------------------------------------
#define RMMOD_COMMAND "/sbin/rmmod"
#define RMMOD_COMMAND_FORMAT RMMOD_COMMAND" %s"


//--------------------------------------------------------------------------------------------------
/**
 * Enum for load status of modules: init, try, installed or removed
 */
//--------------------------------------------------------------------------------------------------
typedef enum {
    STATUS_INIT = 0,    ///< Module is in initialization state
    STATUS_TRY,         ///< Try state before installation or removal
    STATUS_INSTALLED,   ///< If insmod is executed on the module
    STATUS_REMOVED      ///< If rmmod is executed on the module
} ModuleLoadStatus_t;


//--------------------------------------------------------------------------------------------------
/**
 * Legato kernel module object.
 */
//--------------------------------------------------------------------------------------------------
#define KMODULE_OBJECT_COOKIE 0x71a89c35
typedef struct
{
    uint32_t           cookie;                           // KModuleObj_t identifier
    char               *name;                            // Module name
    char               path[LIMIT_MAX_PATH_BYTES];       // Path to module's .ko file
    int                argc;                             // insmod/rmmod argc
    char               *argv[KMODULE_MAX_ARGC];          // insmod/rmmod argv
    le_sls_List_t      reqModuleName;                    // List of required kernel modules
    ModuleLoadStatus_t moduleLoadStatus;                 // Load status of the module
    bool               isLoadManual;                     // is module load set to auto or manual
    le_sls_Link_t      link;                             // link object
    uint32_t           useCount;                         // Counter of usage, safe to remove
                                                         // module when counter is 0
}
KModuleObj_t;


//--------------------------------------------------------------------------------------------------
/**
 * Legato kernel module handler object.
 */
//--------------------------------------------------------------------------------------------------
static struct {
    le_mem_PoolRef_t    modulePool;        // memory pool of KModuleObj_t objects
    le_mem_PoolRef_t    stringPool;        // memory pool of strings (for argv)
    le_mem_PoolRef_t    reqModStringPool;  // memory pool of strings (for argv)
    le_hashmap_Ref_t    moduleTable;       // table for kernel module objects
} KModuleHandler = {NULL, NULL, NULL, NULL};


//--------------------------------------------------------------------------------------------------
/**
 * Free list of module parameters starting from argv[2]
 */
//--------------------------------------------------------------------------------------------------
static void FreeArgvParams(KModuleObj_t *module)
{
    char **p;

    /* Iterate through remaining parameters and free buffers */
    p = module->argv + 2;
    while (NULL != *p)
    {
        le_mem_Release(*p);
        *p = NULL;
        p++;
    }

    /* Reset number of parameters */
    module->argc = 2;
}


//--------------------------------------------------------------------------------------------------
/**
 * Free list of module parameters
 */
//--------------------------------------------------------------------------------------------------
static void ModuleFreeParams(KModuleObj_t *module)
{
    module->argv[0] = NULL; // Contained exec'ed command, not allocated
    module->argv[1] = NULL; // Contained module path/name, not allocated

    FreeArgvParams(module);

    /* Reset number of parameters */
    module->argc = 0;
}


//--------------------------------------------------------------------------------------------------
/**
 * Build and execute the insmod/rmmod command
 */
//--------------------------------------------------------------------------------------------------
static le_result_t ExecuteCommand(int argc, char *argv[])
{
    pid_t pid, p;
    int result;

    /* First argument argv[0] is always the command */
    LE_DEBUG("Execute '%s %s'", argv[0], argv[1]);

    pid = fork();
    LE_FATAL_IF(-1 == pid, "fork() failed. (%m)");

    if (0 == pid)
    {
        /* Child, exec command. */
        execv(argv[0], argv);
        /* Should never be here. */
        LE_FATAL("Failed to run '%s %s'. (%m)", argv[0], argv[1]);
    }

    /* Wait for command to complete; restart on EINTR. */
    while (-1 == (p = waitpid(pid, &result, 0)) && EINTR == errno);
    if (p != pid)
    {
        if (p == -1)
        {
            LE_FATAL("waitpid() failed: %m");
            return LE_FAULT;
        }
        else
        {
            LE_FATAL("waitpid() returned unexpected result %d", p);
            return LE_FAULT;
        }
    }

    /* Check exit status and errors */
    if (WIFSIGNALED(result))
    {
        LE_CRIT("%s was killed by a signal %d.", argv[0], WTERMSIG(result));
        return LE_FAULT;
    }
    else if (WIFEXITED(result) && WEXITSTATUS(result) != EXIT_SUCCESS)
    {
        LE_CRIT("%s exited with error code %d.", argv[0], WEXITSTATUS(result));
        return LE_FAULT;
    }
    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Read the load section to determine if the module is auto or manual load
 */
//--------------------------------------------------------------------------------------------------
static void ModuleGetLoad(KModuleObj_t *module)
{
    char cfgTreePath[LE_CFG_STR_LEN_BYTES];
    le_cfg_IteratorRef_t iter;

    cfgTreePath[0] = '\0';
    le_path_Concat("/", cfgTreePath, LE_CFG_STR_LEN_BYTES,
                   KMODULE_CONFIG_TREE_ROOT, module->name, (char*)NULL);
    iter = le_cfg_CreateReadTxn(cfgTreePath);


    module->isLoadManual = le_cfg_GetBool(iter, "loadManual", false);

    le_cfg_CancelTxn(iter);
}


//--------------------------------------------------------------------------------------------------
/**
 * Populate list of module parameters for argv
 */
//--------------------------------------------------------------------------------------------------
static void ModuleGetParams(KModuleObj_t *module)
{
    char *p;
    char cfgTreePath[LE_CFG_STR_LEN_BYTES];
    char tmp[LE_CFG_STR_LEN_BYTES];
    le_cfg_IteratorRef_t iter;

    cfgTreePath[0] = '\0';
    le_path_Concat("/", cfgTreePath, LE_CFG_STR_LEN_BYTES,
                   KMODULE_CONFIG_TREE_ROOT, module->name, "params", NULL);
    iter = le_cfg_CreateReadTxn(cfgTreePath);

    if (LE_OK != le_cfg_GoToFirstChild(iter))
    {
        LE_INFO("Module %s uses no parameters.", module->name);
        le_cfg_CancelTxn(iter);
        return;
    }

    /* Populate parameters list from configTree; careful not to overrun array */
    do
    {
        /* allocate and clear string buffer */
        p = (char*)le_mem_ForceAlloc(KModuleHandler.stringPool);
        memset(p, 0, STRINGS_MAX_BUFFER_SIZE);
        module->argv[module->argc] = p;

        /* first get the parameter name, append a '=' and advance to end */
        LE_ASSERT_OK(le_cfg_GetNodeName(iter, "", p, LE_CFG_NAME_LEN_BYTES));
        p[strlen(p)] = '=';
        p += strlen(p);

        /* now get the parameter value, should be string */
        LE_ASSERT_OK(le_cfg_GetString(iter, "", tmp, LE_CFG_STR_LEN_BYTES, ""));

        /* enclose the parameter in quotes if it contains white space */
        if (strpbrk(tmp, " \t\n"))
        {
            /* add space for quotes; likely ok to use sprintf, but anyway... */
            snprintf(p, LE_CFG_STR_LEN_BYTES + 2, "\"%s\"", tmp);
            p += strlen(tmp) + 2;
        }
        else
        {
            strcpy(p, tmp);
            p += strlen(tmp);
        }

        /* increment parameter counter */
        module->argc++;
    }
    while((KMODULE_MAX_ARGC > (module->argc + 1)) &&
          (LE_OK == le_cfg_GoToNextSibling(iter)));

    le_cfg_CancelTxn(iter);

    /* Last argument to execv must be null */
    module->argv[module->argc] = NULL;

    if (KMODULE_MAX_ARGC <= module->argc + 1)
        LE_WARN("Parameters list truncated for module '%s'", module->name);
}

//--------------------------------------------------------------------------------------------------
/**
 * Populate list of required kernel modules that a given module depends on
 */
//--------------------------------------------------------------------------------------------------
static void ModuleGetRequiredModules(KModuleObj_t *module)
{
    ModNameNode_t* modNameNodePtr;
    char cfgTreePath[LE_CFG_STR_LEN_BYTES];

    cfgTreePath[0] = '\0';
    le_path_Concat("/", cfgTreePath, LE_CFG_STR_LEN_BYTES,
                   KMODULE_CONFIG_TREE_ROOT, module->name, "requires/kernelModules", NULL);

    le_cfg_IteratorRef_t iter = le_cfg_CreateReadTxn(cfgTreePath);
    if (le_cfg_GoToFirstChild(iter) != LE_OK)
    {
        goto done_deps;
    }

    do
    {
        if (le_cfg_GetNodeType(iter, ".") != LE_CFG_TYPE_STRING)
        {
            LE_WARN("Found non-string type kernel module dependency");
            continue;
        }

        modNameNodePtr = le_mem_ForceAlloc(KModuleHandler.reqModStringPool);
        modNameNodePtr->link = LE_SLS_LINK_INIT;

        le_cfg_GetString(iter, "", modNameNodePtr->modName, sizeof(modNameNodePtr->modName), "");
        if (strncmp(modNameNodePtr->modName, "", sizeof(modNameNodePtr->modName)) == 0)
        {
            LE_WARN("Found empty kernel module dependency");
            le_mem_Release(modNameNodePtr);
            continue;
        }

        le_sls_Queue(&(module->reqModuleName), &(modNameNodePtr->link));
    }
    while(le_cfg_GoToNextSibling(iter) == LE_OK);

done_deps:
    le_cfg_CancelTxn(iter);
    module->moduleLoadStatus = STATUS_INIT;
}


//--------------------------------------------------------------------------------------------------
/**
 * Insert a module to the table with a given module name
 */
//--------------------------------------------------------------------------------------------------
static void ModuleInsert(char *modName)
{
    KModuleObj_t *m;

    /* Allocate module object */
    m = (KModuleObj_t*)le_mem_ForceAlloc(KModuleHandler.modulePool);
    memset(m, 0, sizeof(KModuleObj_t));

    LE_ASSERT_OK(le_path_Concat("/",
                            m->path,
                            LIMIT_MAX_PATH_BYTES,
                            SYSTEM_MODULE_PATH,
                            modName,
                            NULL));

    m->cookie = KMODULE_OBJECT_COOKIE;
    m->name = le_path_GetBasenamePtr(m->path, "/");

    /* Now build a parameter list that will be sent to execv */
    m->argv[0] = NULL;      /* First: reserved for execv command */
    m->argv[1] = m->path;   /* Second: file/module path */
    m->argc = 2;
    m->reqModuleName = LE_SLS_LIST_INIT;
    m->useCount = 0;
    m->isLoadManual = false;

    ModuleGetLoad(m);
    ModuleGetParams(m);          /* Read module parameters from configTree */
    ModuleGetRequiredModules(m); /* Read required kernel modules from configTree */

    //Insert in a hashMap
    le_hashmap_Put(KModuleHandler.moduleTable, m->name, m);
}


//--------------------------------------------------------------------------------------------------
/**
 * For insertion, traverse through the module table and add modules with dependencies to Stack list
 */
//--------------------------------------------------------------------------------------------------
static void TraverseDependencyInsert(le_sls_List_t* ModuleInsertList, KModuleObj_t *m)
{
    LE_ASSERT(m != NULL);

    KModuleObj_t* KModulePtr;
    le_sls_Link_t* modNameLinkPtr;
    ModNameNode_t* modNameNodePtr;

    le_sls_Stack(ModuleInsertList, &(m->link));

    if (m->moduleLoadStatus != STATUS_INSTALLED)
    {
       m->moduleLoadStatus = STATUS_TRY;
    }

    modNameLinkPtr = le_sls_Peek(&(m->reqModuleName));

    while (modNameLinkPtr != NULL)
    {
        modNameNodePtr = CONTAINER_OF(modNameLinkPtr, ModNameNode_t,link);
        KModulePtr = le_hashmap_Get(KModuleHandler.moduleTable, modNameNodePtr->modName);

        TraverseDependencyInsert(ModuleInsertList, KModulePtr);

        modNameLinkPtr = le_sls_PeekNext(&(m->reqModuleName), modNameLinkPtr);
    }
}


//--------------------------------------------------------------------------------------------------
/**
 * insmod the kernel module
 */
//--------------------------------------------------------------------------------------------------
static le_result_t InstallEachKernelModule(KModuleObj_t *m)
{
    le_result_t result;
    le_sls_Link_t *listLink;

    /* The ordered list of required kernel modules to install */
    le_sls_List_t ModuleInsertList = LE_SLS_LIST_INIT;

    TraverseDependencyInsert(&ModuleInsertList, m);

    for (listLink = le_sls_Pop(&ModuleInsertList);
        listLink != NULL;
        listLink = le_sls_Pop(&ModuleInsertList))
    {
        KModuleObj_t *mod = CONTAINER_OF(listLink, KModuleObj_t, link);

        mod->useCount++;

        if (mod->moduleLoadStatus != STATUS_INSTALLED)
        {
            mod->argv[0] = INSMOD_COMMAND;
            result = ExecuteCommand(mod->argc, mod->argv);
            if (result != LE_OK)
            {
                return result;
            }

            mod->moduleLoadStatus = STATUS_INSTALLED;
            LE_INFO("New kernel module %s", mod->name);
        }
    }
    return LE_OK;
}

//--------------------------------------------------------------------------------------------------
/**
 * Traverse through the given list of kernel module names and install each module
 */
//--------------------------------------------------------------------------------------------------
void kernelModules_InsertListOfModules(le_sls_List_t reqModuleName)
{
    KModuleObj_t* m;
    le_result_t result;
    ModNameNode_t* modNameNodePtr;

    le_sls_Link_t* modNameLinkPtr = le_sls_Peek(&reqModuleName);

    while (modNameLinkPtr != NULL)
    {
        modNameNodePtr = CONTAINER_OF(modNameLinkPtr, ModNameNode_t,link);

        m = le_hashmap_Get(KModuleHandler.moduleTable, modNameNodePtr->modName);
        LE_ASSERT(m && KMODULE_OBJECT_COOKIE == m->cookie);

        if (m->isLoadManual ||
            ((!m->isLoadManual) && (m->moduleLoadStatus != STATUS_INSTALLED)))
        {
            result = InstallEachKernelModule(m);
            if (result != LE_OK)
            {
                LE_ERROR("Error in installing module %s", m->name);
                break;
            }
        }

        modNameLinkPtr = le_sls_PeekNext(&reqModuleName, modNameLinkPtr);
    }
}


//--------------------------------------------------------------------------------------------------
/**
 * Iterate through the module table and install kernel module
 */
//--------------------------------------------------------------------------------------------------
static void installModules()
{
    KModuleObj_t *m;
    le_hashmap_It_Ref_t iter;
    le_result_t result;

    /* Iterate through the module table */
    iter = le_hashmap_GetIterator(KModuleHandler.moduleTable);

    while (le_hashmap_NextNode(iter) == LE_OK)
    {
        m = (KModuleObj_t*)le_hashmap_GetValue(iter);
        LE_ASSERT(m && KMODULE_OBJECT_COOKIE == m->cookie);

        if (m->isLoadManual)
        {
            continue;
        }

        result = InstallEachKernelModule(m);
        if (result != LE_OK)
        {
            LE_ERROR("Error in installing module %s", m->name);
            break;
        }
    }
}


//--------------------------------------------------------------------------------------------------
/**
 * Traverse modules configTree (system:/modules) and insmod all modules in the order of dependencies
 */
//--------------------------------------------------------------------------------------------------
void kernelModules_Insert(void)
{
    char modName[LE_CFG_STR_LEN_BYTES];

    le_cfg_IteratorRef_t iter = le_cfg_CreateReadTxn("system:");
    le_cfg_GoToNode(iter, "/modules");
    le_cfg_GoToFirstChild(iter);

    do
    {
        le_cfg_GetNodeName(iter, "", modName, sizeof(modName));

        if (strncmp(modName, "", LE_CFG_STR_LEN_BYTES) == 0)
        {
            LE_WARN("Found empty kernel module node");
            continue;
        }

        /* Check for valid kernel module file extension ".ko" */
        if (le_path_FindTrailing(modName, KERNEL_MODULE_FILE_EXTENSION) != NULL)
        {
            ModuleInsert(modName);
        }
    }
    while (le_cfg_GoToNextSibling(iter) == LE_OK);

    le_cfg_CancelTxn(iter);

    installModules();
}


//--------------------------------------------------------------------------------------------------
/**
 * Release memory taken by kernel modules
 */
//--------------------------------------------------------------------------------------------------
static void ReleaseModulesMemory(void)
{
    KModuleObj_t *m;
    le_hashmap_It_Ref_t iter;

    LE_INFO("Releasing kernel modules memory");

    iter = le_hashmap_GetIterator(KModuleHandler.moduleTable);

    /* Iterate through the kernel module table */
    while(le_hashmap_NextNode(iter) == LE_OK)
    {
        m = (KModuleObj_t*) le_hashmap_GetValue(iter);
        LE_ASSERT(m && KMODULE_OBJECT_COOKIE == m->cookie);

        /* Reset exec arguments */
        ModuleFreeParams(m);
        LE_ASSERT(m == le_hashmap_Remove(KModuleHandler.moduleTable, m->name));
        le_mem_Release(m);
        LE_INFO("Released module '%s'", m->name);
    }
}

//--------------------------------------------------------------------------------------------------
/**
 * For removal, traverse through the module table and add modules with dependencies to Queue list
 */
//--------------------------------------------------------------------------------------------------
static void TraverseDependencyRemove(le_sls_List_t* ModuleRemoveList, KModuleObj_t *m)
{
    LE_ASSERT(m != NULL);

    KModuleObj_t* KModulePtr;
    le_sls_Link_t* modNameLinkPtr;
    ModNameNode_t* modNameNodePtr;

    le_sls_Queue(ModuleRemoveList, &(m->link));

    if (m->moduleLoadStatus != STATUS_REMOVED)
    {
        if ((m->useCount != 0) && (m->moduleLoadStatus == STATUS_INSTALLED))
        {
            LE_DEBUG("Module %s is installed and not yet ready to be removed.", m->name);
        }
        else
        {
            m->moduleLoadStatus = STATUS_TRY;
        }
    }

    modNameLinkPtr = le_sls_Peek(&(m->reqModuleName));

    while (modNameLinkPtr != NULL)
    {
        modNameNodePtr = CONTAINER_OF(modNameLinkPtr, ModNameNode_t,link);
        KModulePtr = le_hashmap_Get(KModuleHandler.moduleTable, modNameNodePtr->modName);

        TraverseDependencyRemove(ModuleRemoveList, KModulePtr);

        modNameLinkPtr = le_sls_PeekNext(&(m->reqModuleName), modNameLinkPtr);
    }
}


//--------------------------------------------------------------------------------------------------
/**
 * rmmod the kernel module
 */
//--------------------------------------------------------------------------------------------------
static le_result_t RemoveEachKernelModule(KModuleObj_t *m)
{
    le_sls_Link_t *listLink;
    le_result_t result;

    /* The ordered list of required kernel modules to remove */
    le_sls_List_t ModuleRemoveList = LE_SLS_LIST_INIT;

    TraverseDependencyRemove(&ModuleRemoveList, m);

    for (listLink = le_sls_Pop(&ModuleRemoveList);
        listLink != NULL;
        listLink = le_sls_Pop(&ModuleRemoveList))
    {
        KModuleObj_t *mod = CONTAINER_OF(listLink, KModuleObj_t, link);

        if (mod->useCount != 0)
        {
            /* Keep decrementing useCount. When useCount = 0, safe to remove module */
            mod->useCount--;
        }

        if ((mod->useCount == 0)
             && (mod->moduleLoadStatus != STATUS_REMOVED))
        {
            /* Populate argv for rmmod */
            mod->argv[0] = RMMOD_COMMAND;
            mod->argv[1] = mod->name;

            FreeArgvParams(mod);

            result = ExecuteCommand(mod->argc, mod->argv);
            if (result != LE_OK)
            {
                return result;
            }

            mod->moduleLoadStatus = STATUS_REMOVED;
            LE_INFO("Removed kernel module %s", mod->name);
        }
    }
    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Traverse through the given list of kernel module names and remove each module
 */
//--------------------------------------------------------------------------------------------------
void kernelModules_RemoveListOfModules(le_sls_List_t reqModuleName)
{
    KModuleObj_t* m;
    le_result_t result;
    ModNameNode_t* modNameNodePtr;
    le_sls_Link_t* modNameLinkPtr = le_sls_Peek(&reqModuleName);

    while (modNameLinkPtr != NULL)
    {
        modNameNodePtr = CONTAINER_OF(modNameLinkPtr, ModNameNode_t,link);
        m = le_hashmap_Get(KModuleHandler.moduleTable, modNameNodePtr->modName);
        LE_ASSERT(m && KMODULE_OBJECT_COOKIE == m->cookie);

        if (m->isLoadManual)
        {
            result = RemoveEachKernelModule(m);
            if (result != LE_OK)
            {
                LE_ERROR("Error in removing module %s", m->name);
                break;
            }
        }

        modNameLinkPtr = le_sls_PeekNext(&reqModuleName, modNameLinkPtr);
    }
}


//--------------------------------------------------------------------------------------------------
/**
 * Remove previously inserted modules in the order of dependencies
 */
//--------------------------------------------------------------------------------------------------
void kernelModules_Remove(void)
{
    KModuleObj_t *m;
    le_hashmap_It_Ref_t iter;
    le_result_t result;

    iter = le_hashmap_GetIterator(KModuleHandler.moduleTable);

    /* Iterate through the kernel module table */
    while (le_hashmap_NextNode(iter) == LE_OK)
    {
        m = (KModuleObj_t*) le_hashmap_GetValue(iter);
        LE_ASSERT(m && KMODULE_OBJECT_COOKIE == m->cookie);

        if (m->isLoadManual)
        {
            continue;
        }

        result  = RemoveEachKernelModule(m);
        if (result != LE_OK)
        {
            LE_ERROR("Error in removing module %s", m->name);
            break;
        }
    }

    ReleaseModulesMemory();
}


//--------------------------------------------------------------------------------------------------
/**
 * Initialize kernel module handler
 */
//--------------------------------------------------------------------------------------------------
void kernelModules_Init(void)
{
    // Create memory pool of kernel modules
    KModuleHandler.modulePool = le_mem_CreatePool("Kernel Module Mem Pool",
                                                  sizeof(KModuleObj_t));
    le_mem_ExpandPool(KModuleHandler.modulePool, KMODULE_DEFAULT_POOL_SIZE);

    // Create memory pool of strings for module parameters
    KModuleHandler.stringPool = le_mem_CreatePool("Module Params Mem Pool",
                                                  STRINGS_MAX_BUFFER_SIZE);
    le_mem_ExpandPool(KModuleHandler.stringPool, STRINGS_DEFAULT_POOL_SIZE);

    // Create memory pool of strings for required kernel module names
    KModuleHandler.reqModStringPool = le_mem_CreatePool("Required Module Mem Pool",
                                                        sizeof(ModNameNode_t));
    le_mem_ExpandPool(KModuleHandler.reqModStringPool, STRINGS_DEFAULT_POOL_SIZE);

    // Note that modules.dep file cannot be used for the time being as it requires kernel changes.
    // This option will be investigated in the future. Also, to support backward compatibility of
    // existing targets, module dependency support without kernel changes is a must.

    // Create table of kernel module objects
    KModuleHandler.moduleTable = le_hashmap_Create("KModule Objects",
                                             31,
                                             le_hashmap_HashString,
                                             le_hashmap_EqualsString);
}

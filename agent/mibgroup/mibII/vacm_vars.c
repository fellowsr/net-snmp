/*
 * SNMPv3 View-based Access Control Model
 */

#include <config.h>

#if HAVE_STDLIB_H
#include <stdlib.h>
#endif
#if HAVE_UNISTD_H
#include <unistd.h>
#endif
#if HAVE_STRING_H
#include <string.h>
#else
#include <strings.h>
#endif
#if HAVE_MALLOC_H
#include <malloc.h>
#endif
#include <ctype.h>
#include <sys/types.h>
#if HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif
#if HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif

#if HAVE_DMALLOC_H
#include <dmalloc.h>
#endif

#if HAVE_NETDB_H
#include <netdb.h>
#endif
#if HAVE_WINSOCK_H
#include <winsock.h>
#endif

#include "mibincl.h"
#include "read_config.h"
#include "agent_read_config.h"
#include "system.h"
#include "vacm.h"
#include "callback.h"
#include "agent_registry.h"
#include "agent_callbacks.h"
#include "vacm_vars.h"
#include "mib_module_config.h"
#ifdef USING_MIBII_SYSORTABLE_MODULE
#if TIME_WITH_SYS_TIME
# include <sys/time.h>
# include <time.h>
#else
# if HAVE_SYS_TIME_H
#  include <sys/time.h>
# else
#  include <time.h>
# endif
#endif
#include "sysORTable.h"
#endif

void
init_vacm_vars (void) 
{

#ifdef USING_MIBII_SYSORTABLE_MODULE
  static oid reg[] = {1,3,6,1,6,3,16,2,2,1};
#endif
  
#define PRIVRW	(SNMPV2ANY | 0x5000)

  struct variable2 vacm_sec2group[] = {
    {SECURITYMODEL, ASN_INTEGER, PRIVRW, var_vacm_sec2group, 1, {1}},
    {SECURITYNAME, ASN_OCTET_STR, PRIVRW, var_vacm_sec2group, 1, {2}},
    {SECURITYGROUP, ASN_OCTET_STR, PRIVRW, var_vacm_sec2group, 1, {3}},
    {SECURITYSTORAGE, ASN_INTEGER, PRIVRW, var_vacm_sec2group, 1, {4}},
    {SECURITYSTATUS, ASN_INTEGER, PRIVRW, var_vacm_sec2group, 1, {5}},
  };

  struct variable2 vacm_access[] = {
    {ACCESSPREFIX, ASN_OCTET_STR, PRIVRW, var_vacm_access, 1, {1}},
    {ACCESSMODEL, ASN_INTEGER, PRIVRW, var_vacm_access, 1, {2}},
    {ACCESSLEVEL, ASN_INTEGER, PRIVRW, var_vacm_access, 1, {3}},
    {ACCESSMATCH, ASN_INTEGER, PRIVRW, var_vacm_access, 1, {4}},
    {ACCESSREAD, ASN_OCTET_STR, PRIVRW, var_vacm_access, 1, {5}},
    {ACCESSWRITE, ASN_OCTET_STR, PRIVRW, var_vacm_access, 1, {6}},
    {ACCESSNOTIFY, ASN_OCTET_STR, PRIVRW, var_vacm_access, 1, {7}},
    {ACCESSSTORAGE, ASN_INTEGER, PRIVRW, var_vacm_access, 1, {8}},
    {ACCESSSTATUS, ASN_INTEGER, PRIVRW, var_vacm_access, 1, {9}},
  };

  struct variable2 vacm_view[] = {
    {VIEWNAME, ASN_OCTET_STR, PRIVRW, var_vacm_view, 1, {1}},
    {VIEWSUBTREE, ASN_OBJECT_ID, PRIVRW, var_vacm_view, 1, {2}},
    {VIEWMASK, ASN_OCTET_STR, PRIVRW, var_vacm_view, 1, {3}},
    {VIEWTYPE, ASN_INTEGER, PRIVRW, var_vacm_view, 1, {4}},
    {VIEWSTORAGE, ASN_INTEGER, PRIVRW, var_vacm_view, 1, {5}},
    {VIEWSTATUS, ASN_INTEGER, PRIVRW, var_vacm_view, 1, {6}},
  };

/* Define the OID pointer to the top of the mib tree that we're
   registering underneath */
  oid vacm_sec2group_oid[] = { OID_VACMGROUPENTRY };
  oid vacm_access_oid[] = { OID_VACMACCESSENTRY};
  oid vacm_view_oid[] = { OID_VACMVIEWENTRY };

  /* register ourselves with the agent to handle our mib tree */
  REGISTER_MIB("mibII/vacm:sec2group", vacm_sec2group, variable2, \
               vacm_sec2group_oid);
  REGISTER_MIB("mibII/vacm:access", vacm_access, variable2, vacm_access_oid);
  REGISTER_MIB("mibII/vacm:view", vacm_view, variable2, vacm_view_oid);

  snmpd_register_config_handler("com2sec", vacm_parse_security,
                                vacm_free_security,"name source community");
  snmpd_register_config_handler("group", vacm_parse_group, vacm_free_group,
                                "name v1|v2c|usm|any security");
  snmpd_register_config_handler("access", vacm_parse_access, vacm_free_access,
                            "name context model level prefx read write notify");
  snmpd_register_config_handler("view", vacm_parse_view, vacm_free_view,
                                "name type subtree [mask]");
  snmpd_register_config_handler("rwcommunity", vacm_parse_simple,
                                NULL,"community [default|hostname|network/bits] [oid]");
  snmpd_register_config_handler("rocommunity", vacm_parse_simple,
                                NULL,"community [default|hostname|network/bits] [oid]");

#ifdef USING_MIBII_SYSORTABLE_MODULE
  register_sysORTable(reg,10,"View-based Access Control Model for SNMP.");
#endif

  /* register ourselves to handle access control */
  snmp_register_callback(SNMP_CALLBACK_APPLICATION, SNMPD_CALLBACK_ACM_CHECK,
                         vacm_in_view_callback, NULL);
}

static struct vacm_securityEntry *securityFirst =0, *securityLast =0;

#define EXAMPLE_NETWORK		"NETWORK"
#define EXAMPLE_COMMUNITY	"COMMUNITY"

void vacm_parse_security (const char *token, 
			  char *param)
{
    char *name, *source, *community;
    const char *mask;
    char *cp;
    struct vacm_securityEntry *sp, se;
    int maskLength, maskBit;
    struct sockaddr_in *srcIp, *srcMask;
    char null[] = "";

    memset (&se, 0 , sizeof se);
    name = strtok(param, "\t\n ");
    if (!name) {
	config_perror("missing NAME parameter");
	return;
    }
    source = strtok(NULL, "\t\n ");
    if (!source) {
	config_perror("missing SOURCE parameter");
	return;
    }
    if ( !strncmp( source, EXAMPLE_NETWORK, strlen(EXAMPLE_NETWORK)) ) {
	config_perror("Example config NETWORK not properly configured");
	return;		/* or exit(1); */
    }
    community = strtok(NULL, "\t\n ");
    if (!community) {
	config_perror("missing COMMUNITY parameter");
	return;
    }
    if ( !strncmp( community, EXAMPLE_COMMUNITY, strlen(EXAMPLE_COMMUNITY)) ) {
	config_perror("Example config COMMUNITY not properly configured");
	return;		/* or exit(1); */
    }
    srcIp   = (struct sockaddr_in*)&(se.sourceIp);
    srcMask = (struct sockaddr_in*)&(se.sourceMask);
    cp = strchr(source, '/');
    if (cp == NULL) cp = null;
    else *cp++ = 0;
    mask = cp;
    if (strcmp("default", source) == 0 || strcmp("0.0.0.0", source) == 0) {
	memset(&(srcIp->sin_addr), 0, sizeof(struct in_addr));
	mask = "0.0.0.0";
    }
    else if ((srcIp->sin_addr.s_addr = inet_addr (source)) == (unsigned) -1) {
	struct hostent *hp = gethostbyname(source);
	if (hp != NULL) {
	    memcpy(&(srcIp->sin_addr), hp->h_addr, 4);
	}
	else {
	    config_perror ("bad source address");
	    return;
	}
    }
    if (*mask == 0) memset (&(srcMask->sin_addr), 0xff, sizeof(struct in_addr));
    else {
	if (strchr(mask, '.')) {
	    if ((srcMask->sin_addr.s_addr = inet_addr(mask)) == (unsigned)-1) {
		config_perror("bad mask");
		return;
	    }
	}
	else {
	    maskLength = atoi(mask);
	    if (maskLength <= 0 || maskLength > 32) {
		config_perror("bad mask length");
		return;
	    }
	    maskBit = 0x80000000L;
	    srcMask->sin_addr.s_addr = 0;
	    while (maskLength--) {
		srcMask->sin_addr.s_addr |= maskBit;
		maskBit >>= 1;
	    }
	    srcMask->sin_addr.s_addr = htonl(srcMask->sin_addr.s_addr);
	}
    }
    if ((srcIp->sin_addr.s_addr & ~srcMask->sin_addr.s_addr) != 0) {
	config_perror("source/mask mismatch");
	return;
    }
    strcpy(se.securityName, name);
    strcpy(se.community, community);
    sp = (struct vacm_securityEntry *)malloc (sizeof *sp);
    *sp = se;
    if (securityFirst != NULL) {
	securityLast->next = sp;
	securityLast = sp;
    }
    else {
	securityFirst = securityLast = sp;
    }
}

void vacm_free_security (void)
{
    struct vacm_securityEntry *sp;
    while ((sp = securityFirst)) {
	securityFirst = sp->next;
	free(sp);
    }
}

void vacm_parse_group (const char *token, 
		       char *param)
{
    char *group, *model, *security;
    int imodel;
    struct vacm_groupEntry *gp = NULL;

    group = strtok (param, " \t\n");
    model = strtok (NULL, " \t\n");
    security = strtok (NULL, " \t\n");

    if (group == NULL || *group == 0) {
	config_perror("missing GROUP parameter");
	return;
    }
    if (model == NULL || *model == 0) {
	config_perror("missing MODEL parameter");
	return;
    }
    if (security == NULL || *security == 0) {
	config_perror("missing SECURITY parameter");
	return;
    }
    if (strcasecmp(model, "any") == 0) imodel = SNMP_SEC_MODEL_ANY;
    else if (strcasecmp(model, "v1") == 0) imodel = SNMP_SEC_MODEL_SNMPv1;
    else if (strcasecmp(model, "v2c") == 0) imodel = SNMP_SEC_MODEL_SNMPv2c;
    else if (strcasecmp(model, "usm") == 0) imodel = SNMP_SEC_MODEL_USM;
    else {
	config_perror("bad security model");
	return;
    }
    gp = vacm_createGroupEntry(imodel, security);
    strcpy (gp->groupName, group);
    gp->storageType = SNMP_STORAGE_PERMANENT;
    gp->status = SNMP_ROW_ACTIVE;
    free (gp->reserved);
    gp->reserved = NULL;
}

void vacm_free_group (void)
{
    vacm_destroyAllGroupEntries();
}

void vacm_parse_access (const char *token, char *param)
{
    char *name, *context, *model, *level, *prefix, *readView, *writeView, *notify;
    int imodel, ilevel;
    struct vacm_accessEntry *ap;

    name = strtok(param, " \t\n");
    if (!name) {
	config_perror("missing NAME parameter");
	return;
    }
    context = strtok(NULL, " \t\n");
    if (!context) {
	config_perror("missing CONTEXT parameter");
	return;
    }
    model = strtok(NULL, " \t\n");
    if (!model) {
	config_perror("missing MODEL parameter");
	return;
    }
    level = strtok(NULL, " \t\n");
    if (!level) {
	config_perror("missing LEVEL parameter");
	return;
    }
    prefix = strtok(NULL, " \t\n");
    if (!prefix) {
	config_perror("missing PREFIX parameter");
	return;
    }
    readView = strtok(NULL, " \t\n");
    if (!readView) {
	config_perror("missing readView parameter");
	return;
    }
    writeView = strtok(NULL, " \t\n");
    if (!writeView) {
	config_perror("missing writeView parameter");
	return;
    }
    notify = strtok(NULL, " \t\n");
    if (!notify) {
	config_perror("missing notifyView parameter");
	return;
    }
    if (strcmp(context, "\"\"") == 0) *context = 0;
    if (strcasecmp(model, "any") == 0) imodel = SNMP_SEC_MODEL_ANY;
    else if (strcasecmp(model, "v1") == 0) imodel = SNMP_SEC_MODEL_SNMPv1;
    else if (strcasecmp(model, "v2c") == 0) imodel = SNMP_SEC_MODEL_SNMPv2c;
    else if (strcasecmp(model, "usm") == 0) imodel = SNMP_SEC_MODEL_USM;
    else {
	config_perror("bad security model");
	return;
    }
    if (strcasecmp(level, "noauth") == 0) ilevel = SNMP_SEC_LEVEL_NOAUTH;
    else if (strcasecmp(level, "noauthnopriv") == 0) ilevel = SNMP_SEC_LEVEL_NOAUTH;
    else if (strcasecmp(level, "auth") == 0) ilevel = SNMP_SEC_LEVEL_AUTHNOPRIV;
    else if (strcasecmp(level, "authnopriv") == 0) ilevel = SNMP_SEC_LEVEL_AUTHNOPRIV;
    else if (strcasecmp(model, "priv") == 0) ilevel = SNMP_SEC_LEVEL_AUTHPRIV;
    else if (strcasecmp(model, "authpriv") == 0) ilevel = SNMP_SEC_LEVEL_AUTHPRIV;
    else {
	config_perror("bad security level");
	return;
    }
    ap = vacm_createAccessEntry (name, context, imodel, ilevel);
    strcpy(ap->readView, readView);
    strcpy(ap->writeView, writeView);
    strcpy(ap->notifyView, notify);
    ap->storageType = SNMP_STORAGE_PERMANENT;
    ap->status = SNMP_ROW_ACTIVE;
    free (ap->reserved);
    ap->reserved = NULL;
}

void vacm_free_access (void)
{
    vacm_destroyAllAccessEntries();
}

void vacm_parse_view (const char *token, 
		      char *param)
{
    char *name, *type, *subtree, *mask;
    int inclexcl;
    struct vacm_viewEntry *vp;
    oid suboid[MAX_OID_LEN];
    size_t suboid_len = 0;
    u_char viewMask[sizeof (vp->viewMask)];
    int i;

    init_mib();
    name = strtok (param, " \t\n");
    if (!name) {
	config_perror("missing NAME parameter");
	return;
    }
    type = strtok (NULL, " \n\t");
    if (!type) {
	config_perror("missing TYPE parameter");
	return;
    }
    subtree = strtok(NULL, " \t\n");
    if (!subtree) {
	config_perror("missing SUBTREE parameter");
	return;
    }
    mask = strtok(NULL, " \t\n");

    if (strcmp(type, "included") == 0) inclexcl = SNMP_VIEW_INCLUDED;
    else if (strcmp(type, "excluded") == 0) inclexcl = SNMP_VIEW_EXCLUDED;
    else {
	config_perror("TYPE must be included/excluded?");
	return;
    }
    suboid_len = MAX_OID_LEN;
    if (!read_objid(subtree, suboid, &suboid_len)) {
	config_perror("bad SUBTREE object id");
	return;
    }
    if (mask) {
	int val;
	i = 0;
	for (mask = strtok(mask, "."); mask; mask = strtok(NULL, ":")) {
	    if (i >= sizeof(viewMask)) {
		config_perror("MASK too long");
		return;
	    }
	    if (sscanf(mask, "%x", &val) == 0) {
		config_perror("invalid MASK");
		return;
	    }
	    viewMask[i] = val;
	    i++;
	}
    }
    else {
	for (i = 0; i < sizeof(viewMask); i++)
	    viewMask[i] = 0xff;
    }
    vp = vacm_createViewEntry(name, suboid, suboid_len);
    memcpy(vp->viewMask, viewMask, sizeof(viewMask));
    vp->viewType = inclexcl;
    vp->viewStorageType = SNMP_STORAGE_PERMANENT;
    vp->viewStatus = SNMP_ROW_ACTIVE;
    free (vp->reserved);
    vp->reserved = NULL;
}

void vacm_free_view (void)
{
    vacm_destroyAllViewEntries();
}

void vacm_parse_simple(const char *token, char *confline) {
  char line[SPRINT_MAX_LEN];
  char community[COMMUNITY_MAX_LEN];
  char theoid[SPRINT_MAX_LEN];
  char viewname[SPRINT_MAX_LEN];
  char addressname[SPRINT_MAX_LEN];
  const char *rw = "none";
  char *cp;
  static int num = 0;
  char com2sec[] = "com2sec";
  char group[] = "group";
  char view[] = "view";
  char access[] = "access";

  cp = copy_word(confline, community);
  if (cp && *cp) {
    cp = copy_word(cp, addressname);
  } else {
    strcpy(addressname, "default");
  }
  if (cp && *cp) {
    cp = copy_word(cp, theoid);
  } else {
    strcpy(theoid, ".1");
  }

  if (strcmp(token,"rwcommunity") == 0)
    rw = viewname;

  /* com2sec mapping */
  /* com2sec anonymousSecNameNUM    ADDRESS  COMMUNITY */
  sprintf(line,"anonymousSecName%03d %s %s", num, addressname, community);
  vacm_parse_security(com2sec,line);

  /* sec->group mapping */
  /* group   anonymousGroupNameNUM  any      anonymousSecNameNUM */
  sprintf(line,"anonymousGroupName%03d any anonymousSecName%03d", num, num);
  vacm_parse_group(group,line);

  /* view definition */
  /* view    anonymousViewNUM       included OID */
  sprintf(viewname,"anonymousView%03d",num);
  sprintf(line,"%s included %s", viewname, theoid);
  vacm_parse_view(view,line);

  /* map everything together */
  /* access  anonymousGroupNameNUM  "" any noauth 0 anonymousViewNUM [none/anonymousViewNUM] [none/anonymousViewNUM] */
  sprintf(line, "anonymousGroupName%03d  \"\" any noauth 0 %s %s %s", num,
          viewname, rw, rw);
  vacm_parse_access(access,line);
  num++;
}

int
vacm_in_view_callback(int majorID, int minorID, void *serverarg,
                      void *clientarg) {
  struct view_parameters *view_parms = (struct view_parameters *) serverarg;
  int retval;
  
  if (view_parms == NULL)
    return 0;
  retval = vacm_in_view(view_parms->pdu, view_parms->name,
                        view_parms->namelen);
  if (retval != 1)
    view_parms->errorcode = retval;
  return retval;
}


/*******************************************************************-o-******
 * vacm_in_view
 *
 * Parameters:
 *	*pdu
 *	*name
 *	 namelen
 *      
 * Returns:
 *	1	On success.
 *	0	Otherwise.
 *
 * Debug output listed as follows:
 *	<securityName> <groupName> <viewName> <viewType>
 */
int vacm_in_view (struct snmp_pdu *pdu,
		  oid *name,
		  size_t namelen)
{
    struct vacm_securityEntry *sp = securityFirst;
    struct vacm_accessEntry *ap;
    struct vacm_groupEntry *gp;
    struct vacm_viewEntry *vp;
    struct sockaddr_in *pduIp = (struct sockaddr_in*)&(pdu->address);
    struct sockaddr_in *srcIp, *srcMask;
    char *vn;
    char *sn;

    if (pdu->version == SNMP_VERSION_1 || pdu->version == SNMP_VERSION_2c) {
	if (snmp_get_do_debugging()) {
            char *buf;
            if (pdu->community) {
                buf = malloc(1+ pdu->community_len);
                memcpy(buf, pdu->community, pdu->community_len);
                buf[pdu->community_len] = '\0';
            } else {
                DEBUGMSGTL(("mibII/vacm_vars", "NULL community"));
                buf = strdup("NULL");
            }
            
	    DEBUGMSGTL(("mibII/vacm_vars", "vacm_in_view: ver=%d, source=%.8x, community=%s\n", pdu->version, pduIp->sin_addr.s_addr, buf));
	    free (buf);
	}

	/* allow running without snmpd.conf */
	if (sp == NULL) {
	    DEBUGMSGTL(("mibII/vacm_vars", "vacm_in_view: accepted with no com2sec entries\n"));
	    switch (pdu->command) {
	    case SNMP_MSG_GET:
	    case SNMP_MSG_GETNEXT:
	    case SNMP_MSG_GETBULK:
		return 1;
	    default:
		return 0;
	    }
	}
	while (sp) {
	    srcIp   = (struct sockaddr_in *)&(sp->sourceIp);
	    srcMask = (struct sockaddr_in *)&(sp->sourceMask);
	    if ((pduIp->sin_addr.s_addr & srcMask->sin_addr.s_addr)
		    == srcIp->sin_addr.s_addr
                && strlen(sp->community) == pdu->community_len
		&& !strncmp(sp->community, (char *)pdu->community, pdu->community_len))
		break;
	    sp = sp->next;
	}
	if (sp == NULL) return 0;
	sn = sp->securityName;
    } else if (pdu->securityModel == SNMP_SEC_MODEL_USM) {
      DEBUGMSG (("mibII/vacm_vars",
                 "vacm_in_view: ver=%d, model=%d, secName=%s\n",
                 pdu->version, pdu->securityModel, pdu->securityName));
      sn = pdu->securityName;
    } else {
	sn = NULL;
    }

    if (sn == NULL) return 0;
    DEBUGMSGTL(("mibII/vacm_vars", "vacm_in_view: sn=%s", sn));

    gp = vacm_getGroupEntry(pdu->securityModel, sn);
    if (gp == NULL) { DEBUGMSG(("mibII/vacm_vars", "\n")); return 0; }
    DEBUGMSG (("mibII/vacm_vars", ", gn=%s", gp->groupName));

    ap = vacm_getAccessEntry(gp->groupName, "", pdu->securityModel,
                             pdu->securityLevel);
    if (ap == NULL) { DEBUGMSG(("mibII/vacm_vars", "\n")); return 0; }

    switch (pdu->command) {
      case SNMP_MSG_GET:
      case SNMP_MSG_GETNEXT:
      case SNMP_MSG_GETBULK:
	vn = ap->readView;
	break;
      case SNMP_MSG_SET:
	vn = ap->writeView;
	break;
      case SNMP_MSG_TRAP:
      case SNMP_MSG_TRAP2:
      case SNMP_MSG_INFORM:
	vn = ap->notifyView;
	break;
      default:
        snmp_log(LOG_ERR, "bad msg type in vacm_in_view: %d\n", pdu->command);
	vn = ap->readView;
    }
    DEBUGMSG (("mibII/vacm_vars", ", vn=%s", vn));

    vp = vacm_getViewEntry (vn, name, namelen);
    if (vp == NULL) { DEBUGMSG(("mibII/vacm_vars", "\n")); return 0; }
    DEBUGMSG(("mibII/vacm_vars", ", vt=%d\n", vp->viewType));

    if (vp->viewType == SNMP_VIEW_EXCLUDED) return 0;

    return 1;

}  /* end vacm_in_view() */


u_char *var_vacm_sec2group(struct variable *vp,
			   oid *name,
			   size_t *length,
			   int exact,
			   size_t *var_len,
			   WriteMethod **write_method)
{
    struct vacm_groupEntry *gp;
    oid *groupSubtree;
    int groupSubtreeLen;
    int secmodel;
    char secname[32], *cp;

    write_method = NULL;
    if (memcmp(name, vp->name, sizeof(oid)*vp->namelen) != 0) {
	memcpy(name, vp->name, sizeof(oid)*vp->namelen);
	*length = vp->namelen;
    }
    if (exact) {
	if (*length < 13) return NULL;

	secmodel = name[11];
	groupSubtree = name+13;
	groupSubtreeLen = *length - 13;
	cp = secname;
	while (groupSubtreeLen-- > 0) {
            if (*groupSubtree > 255)
              return 0; /* illegal value */
	    *cp++ = (char) *groupSubtree++;
	}
	*cp = 0;

	gp = vacm_getGroupEntry(secmodel, secname);
    }
    else {
	secmodel = *length > 11 ? name[11] : 0;
	groupSubtree = name+12;
	groupSubtreeLen = *length - 12;
	cp = secname;
	while (groupSubtreeLen-- > 0) {
            if (*groupSubtree > 255)
              return 0; /* illegal value */
	    *cp++ = (char) *groupSubtree++;
	}
	*cp = 0;
	vacm_scanGroupInit();
	while ((gp = vacm_scanGroupNext()) != NULL) {
	    if (gp->securityModel > secmodel ||
		(gp->securityModel == secmodel && strcmp(gp->securityName, secname) > 0))
		break;
	}
	if (gp) {
	    name[11] = gp->securityModel;
	    *length = 12;
	    cp = gp->securityName;
	    while (*cp) {
		name[(*length)++] = *cp++;
	    }
	}
    }

    if (!gp) return NULL;

    *var_len =sizeof(long_return);
    switch (vp->magic) {
    case SECURITYMODEL:
	long_return = gp->securityModel;
	return (u_char *)&long_return;
    case SECURITYNAME:
	*var_len = gp->securityName[0];
	return (u_char *)&gp->securityName[1];
    case SECURITYGROUP:
	*var_len = strlen(gp->groupName);
	return (u_char *)gp->groupName;
    case SECURITYSTORAGE:
	long_return = gp->storageType;
	return (u_char *)&long_return;
    case SECURITYSTATUS:
	long_return = gp->status;
	return (u_char *)&long_return;
    }
    return NULL;
}

u_char *var_vacm_access(struct variable *vp,
			oid *name,
			size_t *length,
			int exact,
			size_t *var_len,
			WriteMethod **write_method)
{
    struct vacm_accessEntry *gp;
    int secmodel;
    int seclevel;
    char groupName[32];
    char contextPrefix[32];
    oid *op;
    int len;
    char *cp;
    int cmp;

    write_method = NULL;
    if (memcmp(name, vp->name, sizeof(oid)*vp->namelen) != 0) {
	memcpy(name, vp->name, sizeof(oid)*vp->namelen);
	*length = vp->namelen;
    }
    if (exact) {
	if (*length < 15) return NULL;

	op = name+11;
	len = *op++;
	cp = groupName;
	while (len-- > 0) {
            if (*op > 255)
              return 0; /* illegal value */
	    *cp++ = (char) *op++;
	}
	*cp = 0;
	len = *op++;
	cp = contextPrefix;
	while (len-- > 0) {
            if (*op > 255)
              return 0; /* illegal value */
	    *cp++ = (char) *op++;
	}
	*cp = 0;
	secmodel = *op++;
	seclevel = *op++;
	if (op != name + *length) {
	    return NULL;
	}

	gp = vacm_getAccessEntry(groupName, contextPrefix, secmodel, seclevel);
    }
    else {
	secmodel = seclevel = 0;
	groupName[0] = 0;
	contextPrefix[0] = 0;
	op = name+11;
	if (op >= name + *length) {
	}
	else {
	    len = *op;
	    cp = groupName;
	    while (len-- >= 0) {
                if (*op > 255)
                  return 0; /* illegal value */
		*cp++ = (char) *op++;
	    }
	    *cp = 0;
	}
	if (op >= name + *length) {
	}
	else {
	    len = *op;
	    cp = contextPrefix;
	    while (len-- >= 0) {
                if (*op > 255)
                  return 0; /* illegal value */
		*cp++ = (char) *op++;
	    }
	    *cp = 0;
	}
	if (op >= name + *length) {
	}
	else {
	    secmodel = *op++;
	}
	if (op >= name + *length) {
	}
	else {
	    seclevel = *op++;
	}
	vacm_scanAccessInit();
	while ((gp = vacm_scanAccessNext()) != NULL) {
	    cmp = strcmp(gp->groupName, groupName);
	    if (cmp > 0) break;
	    if (cmp < 0) continue;
	    cmp = strcmp(gp->contextPrefix, contextPrefix);
	    if (cmp > 0) break;
	    if (cmp < 0) continue;
	    if (gp->securityModel > secmodel) break;
	    if (gp->securityModel < secmodel) continue;
	    if (gp->securityLevel > seclevel) break;
	}
	if (gp) {
	    *length = 11;
	    cp = gp->groupName;
	    do {
		name[(*length)++] = *cp++;
	    } while (*cp);
	    cp = gp->contextPrefix;
	    do {
		name[(*length)++] = *cp++;
	    } while (*cp);
	    name[(*length)++] = gp->securityModel;
	    name[(*length)++] = gp->securityLevel;
	}
    }

    if (!gp) return NULL;

    *var_len =sizeof(long_return);
    switch (vp->magic) {
    case ACCESSMATCH:
	long_return = gp->contextMatch;
	return (u_char *)&long_return;
    case ACCESSLEVEL:
	long_return = gp->securityLevel;
	return (u_char *)&long_return;
    case ACCESSMODEL:
	long_return = gp->securityModel;
	return (u_char *)&long_return;
    case ACCESSPREFIX:
	*var_len = *gp->contextPrefix;
	return (u_char *)&gp->contextPrefix[1];
    case ACCESSREAD:
	*var_len = strlen(gp->readView);
	return (u_char *)gp->readView;
    case ACCESSWRITE:
	*var_len = strlen(gp->writeView);
	return (u_char *)gp->writeView;
    case ACCESSNOTIFY:
	*var_len = strlen(gp->notifyView);
	return (u_char *)gp->notifyView;
    case ACCESSSTORAGE:
	long_return = gp->storageType;
	return (u_char *)&long_return;
    case ACCESSSTATUS:
	long_return = gp->status;
	return (u_char *)&long_return;
    }
    return NULL;
}

u_char *var_vacm_view(struct variable *vp,
		      oid *name,
		      size_t *length,
		      int exact,
		      size_t *var_len,
		      WriteMethod **write_method)
{
    struct vacm_viewEntry *gp;
    char viewName[32];
    oid subtree[MAX_OID_LEN];
    size_t subtreeLen = 0;
    oid *op, *op1;
    int len;
    char *cp;
    int cmp;

    write_method = NULL;
    if (memcmp(name, vp->name, sizeof(oid)*vp->namelen) != 0) {
	memcpy(name, vp->name, sizeof(oid)*vp->namelen);
	*length = vp->namelen;
    }
    if (exact) {
	if (*length < 15) return NULL;

	op = name+12;
	len = *op++;
	cp = viewName;
	while (len-- > 0) {
            if (*op > 255)
              return 0; /* illegal value */
	    *cp++ = (char) *op++;
	}
	*cp = 0;
	len = *length - (op - name);
	op1 = subtree;
	while (len-- > 0) {
	    *op1++ = *op++;
	    subtreeLen++;
	}
	if (op != name + *length) {
	    return NULL;
	}

	gp = vacm_getViewEntry(viewName, subtree, subtreeLen);
    }
    else {
	viewName[0] = 0;
	op = name+12;
	if (op >= name + *length) {
	}
	else {
	    len = *op;
	    cp = viewName;
	    while (len-- >= 0) {
                if (*op > 255)
                  return 0; /* illegal value */
		*cp++ = (char) *op++;
	    }
	    *cp = 0;
	}
	if (op >= name + *length) {
	}
	else {
	    len = *length - (op - name);
	    op1 = subtree;
	    while (len-- >= 0) {
		*op1++ = *op++;
		subtreeLen++;
	    }
	}
	vacm_scanViewInit();
	while ((gp = vacm_scanViewNext()) != NULL) {
	    cmp = strcmp(gp->viewName, viewName);
	    if (cmp > 0) break;
	    if (cmp < 0) continue;
	}
	if (gp) {
	    *length = 12;
	    cp = gp->viewName;
	    do {
		name[(*length)++] = *cp++;
	    } while (*cp);
	    op1 = gp->viewSubtree;
	    len = gp->viewSubtreeLen;
	    do {
		name[(*length)++] = *op1++;
	    } while (len-- > 0);
	}
    }

    if (!gp) return NULL;

    *var_len =sizeof(long_return);
    switch (vp->magic) {
    case VIEWNAME:
	*var_len = gp->viewName[0];
	return (u_char *)&gp->viewName[1];
    case VIEWSUBTREE:
	*var_len = gp->viewSubtreeLen*sizeof(oid);
	return (u_char *)gp->viewSubtree;
    case VIEWMASK:
	*var_len = (gp->viewSubtreeLen + 7) / 8;
	return (u_char *)gp->viewMask;
    case VIEWTYPE:
	long_return = gp->viewType;
	return (u_char *)&long_return;
    case VIEWSTORAGE:
	long_return = gp->viewStorageType;
	return (u_char *)&long_return;
    case VIEWSTATUS:
	long_return = gp->viewStatus;
	return (u_char *)&long_return;
    }
    return NULL;
}

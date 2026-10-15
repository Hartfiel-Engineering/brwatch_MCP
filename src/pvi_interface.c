/*-------------------------------------------------------------------
Module   : pvi_interface.c
 -----------------------------------------------------------------*/

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <direct.h>
#include "main.h"
#include "settings.h"
#include "cpusearch.h"
#include "stringtools.h"
#include "logger.h"
#include "report.h"


#define PVILOGFILE "pvilog.txt"
#define WATCH_FILE_VERSION 	"1.0"
#define MAX(x,y) ((x)>(y)?(x):(y))
#define MAX_SUPPORTED_DEVICES 256
#define MAX_SUPPORTED_CPUS 256


static int g_ansl = 0;


/* Prototypen */
static void WINAPI PviCallback (WPARAM, LPARAM, LPVOID, DWORD, T_RESPONSE_INFO* );
static void CALLBACK PviCyclicRequests( HWND hwnd, UINT uMsg, UINT idEvent, DWORD dwTime );
static void AddRowToLog( char*, int );
static void DeleteLog(void);
static void CreateUniqueObjectName( char *name, char *descriptor );

static PVIOBJECT *AddPviObject(PVIOBJECT *, BOOL);
static PVIOBJECT *UnlinkPviObject( PVIOBJECT *object );
static PVIOBJECT *RemovePviObject(PVIOBJECT *);
static PVIOBJECT *PviReadPvarList( PVIOBJECT * object );
static PVIOBJECT *PviReadCPUList(PVIOBJECT *object );
static PVIOBJECT *PviReadTaskList(PVIOBJECT *object);
static PVIOBJECT *PviReadStructElements( PVIOBJECT *pvar );
static int PviReadAll( DWORD linkid, int access, char **out );
static PVIOBJECT *FindPviObjectByLinkId( DWORD );
static void PviReadDataType( PVIOBJECT *object );

/* */


static char *AddPviDatatype( char * t );
static int GetPviLicenceInfo(void );

/* global variable */
static char tempstring[256]; // save stack space
static UINT_PTR timerid_cyclic_requests;
static BOOL PLCDataChangeEvents;  // PLC- event mode

/* list for PVI objects */
static PVIOBJECT pvirootobject;
static PVIOBJECT *readpointer_pviobject; // zum Auslesen der Pvi- Objektliste

/* list for structure names  */
static PVISTRUCTNAMES structnamesroot = { "DATATYPE", NULL };


/* read the INI file */

static void ReadSettings(void) {
	PVIOBJECT tempobject, *pObject;
	PVIOBJECT *deviceObject;
	int numberOfDevices = 0;
	int numberOfCpus = 0;
	char section[20];
	char key[20];
	char buffer[80];

	deviceObject = calloc( MAX_SUPPORTED_DEVICES, sizeof(PVIOBJECT));
	if( deviceObject == NULL) {
		ReportError( REPORT_ERROR, "Reading *.ini file", "Out of memory !" );
		return;
	}


	memset(&tempobject, 0, sizeof(PVIOBJECT));

	sprintf( section, "General" );
	sprintf( key, "PLCDataChangeEvents" );
	strcpy( buffer, "" );
	GetPrivateProfileString(section, key, "", buffer, sizeof(buffer), SettingsGetFileName());
	PLCDataChangeEvents = atoi(buffer);



	// read devices from brwatch.ini
	pObject = deviceObject;
	numberOfDevices = 0;
	for (unsigned dev = 1; dev <= MAX_SUPPORTED_DEVICES; ++dev) {
		sprintf(section, "DEVICE%u", dev);
		strcpy(key, "broadcast");
		GetPrivateProfileString(section, key, "255.255.255.255", buffer, sizeof(buffer), SettingsGetFileName());
		tempobject.ex.dev.broadcast = inet_addr(buffer);

		strcpy(key, "name");
		strcpy( buffer, "" );
		GetPrivateProfileString(section, key, "", buffer, sizeof(buffer), SettingsGetFileName());

		strcpy(key, "descriptor");
		strcpy( buffer, "" );
		GetPrivateProfileString(section, key, "", buffer, sizeof(buffer), SettingsGetFileName());
		if (strlen(buffer) != 0) {
			// create DEVICE object
			strcpy(tempobject.descriptor, buffer);
			CreateUniqueObjectName(tempstring, tempobject.descriptor);
			snprintf(tempobject.name, sizeof(tempobject.name), "@Pvi/LNBRWATCH/DEV%s", tempstring);
			tempobject.type = POBJ_DEVICE;
			tempobject.ex.dev.number = dev;
			AddPviObject(&tempobject, TRUE);
			// store device's information for later use
			memcpy( pObject, &tempobject, sizeof(PVIOBJECT));
			++pObject;
			++numberOfDevices;
		}
	}

	if( numberOfDevices == 0 ) {
		ReportError( REPORT_ERROR, "Reading *.ini file", "No PVI devices (interfaces) configured in .ini file !" );
		return;
	}


	// read CPUs from brwatch.ini
	numberOfCpus = 0;
	for (unsigned cpu = 1; cpu <= MAX_SUPPORTED_CPUS; ++cpu) {
		char deviceName[256];
		memset(&tempobject, 0, sizeof(tempobject));
		sprintf(section, "CPU%u", cpu);
		strcpy(key, "descriptor");
		strcpy( buffer, "" );
		GetPrivateProfileString(section, key, "", buffer, sizeof(buffer), SettingsGetFileName());
		strcpy(tempobject.descriptor, buffer);

		strcpy(key, "device");
		strcpy( buffer, "" );
		GetPrivateProfileString(section, key, "", buffer, sizeof(buffer), SettingsGetFileName());

		if( strlen(buffer) != 0 ) {
			int deviceNumber = atoi(buffer);
			/* get device's descriptor */
			pObject = deviceObject;
			for( unsigned dev = 0; dev < numberOfDevices; ++dev, ++pObject) {
				if( deviceNumber == pObject->ex.dev.number ) {
					// get device's PVI path name
					char devicename[256];
					CreateUniqueObjectName(devicename, pObject->descriptor);
					// create CPU object
					CreateUniqueObjectName(tempstring, tempobject.descriptor);
					snprintf( tempobject.name, sizeof(tempobject.name), "@Pvi/LNBRWATCH/DEV%s/CPU%s", devicename, tempstring);
					tempobject.type = POBJ_CPU;
					AddPviObject(&tempobject, TRUE);
					++numberOfCpus;
					break;
				}
			}
		}
	}
	
	if( numberOfCpus == 0 ) {
		ReportError( REPORT_ERROR, "Reading *.ini file", "No CPUs configured in .ini file !" );
		return;
	}

	free( deviceObject );


}


/* create unique name from descriptor */

static void CreateUniqueObjectName( char *name, char *descriptor ) {
	while( *descriptor ) {
		if( *descriptor == ' ' )
			++descriptor;
		else if( *descriptor == '/' ) {
			*name++ = '#';
			++descriptor;
		} else if( *descriptor == '.' ) {
			*name ++ = '_';
			++descriptor;
		} else {
			*name++ = toupper(*descriptor++);
		}
	}
	*name = 0;
}


/* add object to list, expects pointer to temporary object from which parameters are copied
returns pointer to new object */

PVIOBJECT *AddPviObject(PVIOBJECT * tempobject, BOOL create ) {

	PVIOBJECT *lastobject;
	PVIOBJECT *newobject;
	char descriptor[256];
	int result = 0;
	BOOL already_exist;   // object already exist
	char *event_mask;

	already_exist = FALSE;

	if (tempobject == NULL) 			// object valid ?
		return NULL;

	// search for last object in list
	lastobject = &pvirootobject;
	while (lastobject->next != NULL) {
		lastobject = lastobject->next;
		if (!strcmp(tempobject->name, lastobject->name) ) {	// does object already exist ?
			already_exist = TRUE;
			break;
		}
	}


	// allocate memory for new object
	if (already_exist == TRUE) {
		newobject = lastobject;
	} else {
		if ((newobject = (PVIOBJECT *)malloc(sizeof(PVIOBJECT))) != NULL) {
			memcpy( newobject, tempobject, sizeof(PVIOBJECT) ); // copy everything for for first
			memset( &newobject->gui_info, 0, sizeof(newobject->gui_info) );
			newobject->gui_info.display_as_decimal = 1;
			newobject->gui_info.display_as_hex = 0;
			newobject->gui_info.display_as_binary = 0;
			newobject->gui_info.display_as_char = 0;
			newobject->watchsort = -1;
			newobject->next = NULL;	// pointer to next object
			lastobject->next = newobject;	// enter new object in list

		} else {
			AddRowToLog("AddPviObject:malloc()", -1);
			return NULL;
		}
	}

	// create variable in PVI manager if necessary
	if (create) {
		strcpy(descriptor, "CD=\"");
		strcat(descriptor, newobject->descriptor);
		strcat(descriptor, "\"");
		event_mask = "Ev=e";
		if( newobject->type == POBJ_PVAR ) {
			if( PLCDataChangeEvents && newobject->ex.pv.dimension <= 1 && newobject->ex.pv.type != BR_STRUCT
			        && newobject->ex.pv.type != BR_STRING && newobject->ex.pv.type != BR_WSTRING ) {
				strcat(descriptor, " RF=50 AT=rwe" );
			} else {
				strcat(descriptor, " RF=50 " );
			}
			event_mask = "Ev=ed";
		}

		result = PviCreate(&newobject->linkid, newobject->name, newobject->type, descriptor, PviCallback, SET_PVICALLBACK_DATA, 0, event_mask );

		if( result ) {
			memset( tempstring, 0, sizeof(tempstring) );
			snprintf(tempstring, sizeof(tempstring), "AddPviObject:PviCreate \"%s\"", tempobject->name);
			AddRowToLog(tempstring, result);
			return NULL;
		} else {
			if( newobject->type == POBJ_PVAR ) {
				PviReadRequest( newobject->linkid, POBJ_ACC_DATA, PviCallback, SET_PVICALLBACK_DATA, 0 );
				// read variable once
			}
		}
	}

	return newobject;

}


/* remove link to object */

static PVIOBJECT *UnlinkPviObject( PVIOBJECT *object ) {
	PVIOBJECT *find;
	int result;

	if( object == NULL )
		return NULL;

	GetNextPviObject(TRUE);

	while( (find = GetNextPviObject(FALSE)) != NULL ) {
		if( find == object ) {
			result = PviUnlink( object->linkid );
			if( result == 0 ) {
				object->linkid =0;
				return object;
			} else {
				AddRowToLog( "UnlinkPviObject", result );
				return NULL;
			}
		}
	}
	return NULL;
}


/* remove object from list */

static PVIOBJECT *RemovePviObject(PVIOBJECT * object ) {
	PVIOBJECT *prevobject, *find;

	prevobject = &pvirootobject;

	// search for predecessor...
	while( prevobject->next != NULL ) {
		find = prevobject->next;

		//if( !strcmp( find->name, object->name ) ) {
		if( find == object ) {
			PviUnlink(object->linkid);	// unlink PVI object
			object->linkid = 0;
			// ignore error since PVI manager could have already deleted childs

			prevobject->next = object->next;	// close gap in linked list


			if( object->type == POBJ_PVAR && object->ex.pv.pvalue != NULL ) { // free memory for watch if necessary
				free(object->ex.pv.pvalue);
				object->ex.pv.pvalue = NULL;
			}
			free(object);					  	// free memory
			return(prevobject);
		}
		prevobject = prevobject->next;		  	// next object
	}

	return( prevobject );
}

/* search for object with link id, expects PVI link id, returns object or NULL if not found */

static PVIOBJECT *FindPviObjectByLinkId( DWORD linkid ) {
	PVIOBJECT *object;

	object = &pvirootobject;
	while( object->next != NULL ) {	// until the end
		object = object->next;
		if( object->linkid == linkid ) {
			return object;
		}
	}
	return NULL;	// object not found
}



/* search for object by name, expects name of object */

PVIOBJECT *FindPviObjectByName( char *name ) {
	PVIOBJECT *object;

	if( name != NULL ) {
		object = &pvirootobject;
		while( object->next != NULL ) {	// until the end
			object = object->next;
			if( strcmp( object->name, name ) == 0 ) {
				return object;
			}
		}
	}
	return NULL;	// object not found
}


/* adds object to watch list */

PVIOBJECT *WatchPviObject(PVIOBJECT * object, BOOL watch) {
	if (object != NULL) {
		switch (object->type) {

			case POBJ_PVAR:
				if (watch) {
					object = AddPviObject( object, TRUE );
					if( object != NULL ) {
						object->ex.pv.pvalue = malloc(object->ex.pv.length); // allocate memeory
						if( object->ex.pv.pvalue ) {
							if( object->ex.pv.type == BR_REAL ) {
								* ((float*)object->ex.pv.pvalue) = 0.0;  /* initialize PV value */
							} else if( object->ex.pv.type == BR_LREAL ) {
								* ((double*)object->ex.pv.pvalue) = 0.0;  /* initialize PV value */
							} else {
								memset( object->ex.pv.pvalue, 0, object->ex.pv.length); /* initialize PV value */
							}
						}
					}
					return( object );
				} else {
					UnlinkPviObject(object);
					object->watchsort = -1;
					if( object->ex.pv.pvalue != NULL ) {
						free(object->ex.pv.pvalue);
						object->ex.pv.pvalue = NULL;
					}
				}
				return (object);
				break;

			default:
				return object;
		}
	}
	return NULL;

}



/* get next object in list, returns next object or first one if 'first' is set to 1 */

PVIOBJECT* GetNextPviObject( BOOL first  ) {
	if( first )
		return( readpointer_pviobject = &pvirootobject );


	if( readpointer_pviobject != NULL ) {
		if( readpointer_pviobject->next != NULL ) {
			readpointer_pviobject = readpointer_pviobject->next;
			return( readpointer_pviobject );
		}
	}
	return NULL;
}



/* search for child object, return first child if 'first' is set to 1 */

PVIOBJECT *FindPviChildObject(PVIOBJECT * object, BOOL first ) {
	static PVIOBJECT *find=NULL;
	size_t length;

	if (object != NULL) {
		if( first )
			find = &pvirootobject;

		length =  strlen(object->name);

		while (find->next != NULL) {
			find = find->next;
			if (strncmp(object->name, find->name, length ) == 0) {
				if( find->name[length] == '/' || find->name[length] == '[' || find->name[length] == '.' )
					if( strrchr( find->name+length+1, '.' )== NULL &&
					        strrchr( find->name+length+1, '/' )== NULL )  // no grandchilds
						return (find);
			}
		}
	}

	return NULL;
}



/* determines parent / child relationship */

BOOL IsPviObjectParentOf( PVIOBJECT *parent, PVIOBJECT * child ) {
	size_t length;


	if( parent == NULL || child == NULL )
		return FALSE;

	length = strlen(parent->name);

	if( strncmp( parent->name, child->name, length ) == 0 ) {
		if( parent != child )
			return TRUE;
	}
	return FALSE;

}


/* count number of objects */

int GetNumberOfPviObjects( void ) {
	PVIOBJECT *p;
	int i=0;

	p =pvirootobject.next;

	while( p != NULL ) {
		++i;
		p = p->next;
	}
	return(i);
}


/* global PVI event */

static void WINAPI Pvi_GlobalEvent(WPARAM wParam, LPARAM lParam, LPVOID pData, DWORD DataLen, T_RESPONSE_INFO * pInfo) {
	PVIOBJECT po, *ppo;


	switch (pInfo->nType) {
		case POBJ_EVENT_PVI_CONNECT:

			break;

		case POBJ_EVENT_PVI_DISCONN:
			AddRowToLog( "POBJ_EVENT_PVI_DISCONN", -1 );
			break;

		case POBJ_EVENT_PVI_ARRANGE: {
			char section[20];
			char key[20];
			char buffer[80];
			sprintf( section, "General" );
			sprintf( key, "Ansl" );
			strcpy( buffer, "" );
			GetPrivateProfileString(section, key, "", buffer, sizeof(buffer), SettingsGetFileName());
			g_ansl = atoi(buffer);
		}

		AddRowToLog("POBJ_EVENT_PVI_ARRANGE", 0);
		ppo = &po;
		strcpy( po.name, "@Pvi/LNBRWATCH");
		if( g_ansl ) {
			strcpy( po.descriptor,"LnAnsl");
		} else {
			strcpy( po.descriptor,"LnIna2");
		}
		po.type = POBJ_LINE;
		AddPviObject( ppo, 1 );
		ReadSettings();

		if( pvi_interface_notify.cbobjects_valid != NULL ) {
			pvi_interface_notify.cbobjects_valid();
		}
		break;
	}

}


/* PVI callback */

static void WINAPI PviCallback (WPARAM wParam, LPARAM lParam, LPVOID pData, DWORD DataLen, T_RESPONSE_INFO* pInfo) {
	PVIOBJECT *object;
	static char value[128];
	char *s, *d;

	switch( pInfo->nType ) {
		case POBJ_EVENT_DATA:			// data change event
			if( DataLen == 0 )			// no data
				return;
			object = FindPviObjectByLinkId(pInfo->LinkID);
			if( object != NULL ) {
				if( object->type == POBJ_PVAR ) {
					switch( object->ex.pv.type ) {
						case BR_STRING:
						case BR_WSTRING:
							if( object->ex.pv.pvalue != NULL )
								memcpy( object->ex.pv.pvalue, pData, MAX(DataLen,object->ex.pv.length) );
							break;

						case BR_USINT:
						case BR_SINT:
						case BR_BOOL:
							if( object->ex.pv.pvalue != NULL )
								memcpy( object->ex.pv.pvalue, pData, 1 );
							break;

						case BR_UINT:
						case BR_INT:
							if( object->ex.pv.pvalue != NULL )
								memcpy( object->ex.pv.pvalue, pData, 2 );
							break;

						case BR_UDINT:
						case BR_DINT:
						case BR_DATI:
						case BR_TOD:
						case BR_DATE:
						case BR_TIME:
						case BR_REAL:
							if( object->ex.pv.pvalue != NULL )
								memcpy( object->ex.pv.pvalue, pData, 4 );
							break;

						case BR_LREAL:
							if( object->ex.pv.pvalue != NULL )
								memcpy( object->ex.pv.pvalue, pData, 8 );
							break;

						default:
							break;



					}
					if( object->error ) { // reset error state
						object->error = 0;
						if( pvi_interface_notify.cberror_changed != NULL )
							pvi_interface_notify.cberror_changed( object, 0 );
					}
					if( pvi_interface_notify.cbdata_changed != NULL )
						pvi_interface_notify.cbdata_changed( object );

					if( object->ex.pv.pvalue != NULL ) {
						object->ex.pv.value_changed = 1; /* value was changed */
						LoggerDataChanged( object );
					}

				}
			}
			break;


		case POBJ_EVENT_ERROR:			// change of error state
			object = FindPviObjectByLinkId(pInfo->LinkID);
			if( object != NULL ) {
				object->error = pInfo->ErrCode;

				if( pvi_interface_notify.cberror_changed != NULL )
					pvi_interface_notify.cberror_changed( object, pInfo->ErrCode );
			}

			break;


		case POBJ_ACC_STATUS:			//change of state
			if( pInfo->ErrCode != 0 )
				return;
			object = FindPviObjectByLinkId(pInfo->LinkID);
			if( object != NULL ) {
				if( pvi_interface_notify.cbdata_changed != NULL ) {
					s = (char*) pData;
					d = value;

					*d = 0;
					while( *s ) {
						if( FindToken( &s, "ST=" )) {
							while( (*d++ = *s++) != 0 );
							*d = 0;
							break;
						}
						++s;
					}


					switch( object->type ) {
						case POBJ_CPU:
							strncpy( object->ex.cpu.status, value, sizeof(object->ex.cpu.status)-1);
							if( strcmp( value, KWSTATUS_WARMSTART ) == 0 || strcmp( value, KWSTATUS_COLDSTART ) == 0 ) {
								object->ex.cpu.running = TRUE;
							} else {
								object->ex.cpu.running = FALSE;
							}
							break;

						case POBJ_TASK:
							strncpy( object->ex.task.status, value, sizeof(object->ex.task.status)-1);
							break;

						default:
							break;
					}
					pvi_interface_notify.cbdata_changed( object );
				}

			}
			break;


		case POBJ_ACC_DATE_TIME:		// read clock
			object = FindPviObjectByLinkId(pInfo->LinkID);
			if( object != NULL ) {
				if( object->type == POBJ_CPU ) {
					if( DataLen > sizeof(struct tm) )
						DataLen = sizeof(struct tm);
					memcpy( &object->ex.cpu.rtc_time, pData, DataLen );
					// not notification since POBJ_ACC_STATUS does that already...
				}
			}
			break;
	}

}



/* is used to send cyclic requests, since not all events are supported for all access types. */
static void CALLBACK PviCyclicRequests( HWND hwnd, UINT uMsg, UINT idEvent, DWORD dwTime ) {
	PVIOBJECT *object;
	static int zyklus_readpv = 0;
	static int zyklus_getlicenseinfo = 0;

	if( ++zyklus_readpv > 4 ) {
		zyklus_readpv = 0;
	}

	// read PVI license information
	if( zyklus_getlicenseinfo < 10) {
		if( ++zyklus_getlicenseinfo >= 10 ) {
			if( GetPviLicenceInfo() == PVIWORK_STATE_LOCKED ) {
				ReportError( REPORT_ERROR, "Error Pvi License", "Sorry PVI is locked.\nProgram must shut down !" );
				SendMessage( MainWindowGetHandle(), WM_CLOSE, 0, 0 );
			} else {
				zyklus_getlicenseinfo = 0;
			}
		}
	}

	object = GetNextPviObject( TRUE );

	while( (object = GetNextPviObject(FALSE)) != NULL ) {
		if( object->linkid == 0 || object->error != 0 )
			continue;

		switch( object->type ) {
			case POBJ_CPU:
				PviReadRequest(object->linkid, POBJ_ACC_STATUS, PviCallback, SET_PVICALLBACK_DATA, object->linkid );
				PviReadRequest(object->linkid, POBJ_ACC_DATE_TIME, PviCallback, SET_PVICALLBACK_DATA, object->linkid );
				break;

			case POBJ_TASK:
				PviReadRequest(object->linkid, POBJ_ACC_STATUS, PviCallback, SET_PVICALLBACK_DATA, object->linkid );
				break;

			default:
				break;

		}
	}

}


static int GetPviLicenceInfo( void ) {
	DWORD LinkId;
	long result;
	T_PVI_INFO_LICENCE license;

	memset( &license, 0, sizeof(license ) );
	result = PviLink( &LinkId, "@Pvi", PVI_HMSG_NIL, 0, 0, 0 );

	if( result == 0 ) {
		result = PviRead( LinkId, POBJ_ACC_INFO_LICENCE, 0, 0, &license, sizeof(license) );
	}

	if( result == 12059 ) {
		return PVIWORK_STATE_LOCKED;
	}

	else if( result != 0 ) {
		AddRowToLog( "GetPviLicenceInfo()", result );
	}

	PviUnlink(LinkId);

	return license.PviWorkState[0];
}


/* start PVI connection */
int StartPvi( void ) {
	int result;

	memset( &pvirootobject, 0, sizeof(PVIOBJECT) );
	strcpy( pvirootobject.name, "@Pvi");
	strcpy( pvirootobject.descriptor, "");
	pvirootobject.type = POBJ_PVI;
	pvirootobject.next = NULL;



	DeleteLog(); // delete PVI log file

	// write PVI version to logfile
	strcpy( g_PVIVersionString, "" );
	PviGetVersion( g_PVIVersionString, sizeof(g_PVIVersionString) );
	AddRowToLog( g_PVIVersionString, 0 );

	//start PVI
	{
		char buffer[80];

		memset( buffer, 0, sizeof(buffer) );
		GetPrivateProfileString( "REMOTE", "parameter", "COMT=10 AS=1 PT=20", buffer, sizeof(buffer), SettingsGetFileName());
		result = PviInitialize( 0, 0, buffer, NULL );
	}

	if( result ) {
		AddRowToLog( "PviInitialize", result );
		return result;
	}




	// global events
	result = PviSetGlobEventMsg( POBJ_EVENT_PVI_ARRANGE, Pvi_GlobalEvent, SET_PVICALLBACK_DATA, 0 );

	if( result ) {
		AddRowToLog( "PviSetGlobEventMsg", result );
		return result;
	}


	// timer for cyclic requests
	timerid_cyclic_requests = SetTimer( NULL, 0, 500, (TIMERPROC) PviCyclicRequests );


	return 0; // Alles OK
}


/* stop PVI */
int StopPvi(void) {

	int result;
	PVIOBJECT *prevobject;

	// kill timer for cyclic requests
	KillTimer( NULL, timerid_cyclic_requests );

	// delete all objects
	prevobject = &pvirootobject;
	while (prevobject->next != NULL) {
		prevobject = RemovePviObject(prevobject->next);
	}


	result = PviDeinitialize();

	// delete all data types
	AddPviDatatype( NULL );

	AddRowToLog("PviDeinitialize", result);
	return result;
}


/* =================================================================================
   functions to work with data types
   ==================================================================================
*/


static char *AddPviDatatype( char * t ) {
	size_t length;
	PVISTRUCTNAMES *p, *kill;

	if( t == NULL ) { // delete all data types
		p = structnamesroot.next	;
		while( p != NULL ) {
			kill = p;
			p = kill->next;
			if( kill->pname != NULL )
				free(kill->pname);
			free(kill);
		}
		structnamesroot.next = NULL;
		return ("nil!");
	}

	length = strlen(t); // length of data type's name

	// search for data type
	p = &structnamesroot;
	while( p->next != NULL ) {
		p = p->next;
		if( p->pname != NULL ) {
			if( !strcmp( p->pname, t ) ) { // already exists
				return( p->pname );
			}
		} else {
			return ("ERROR!");
		}
	}

	// insert data type
	p->next = malloc( sizeof(PVISTRUCTNAMES) );
	if( p->next != NULL ) {
		p = p->next;
		p->pname = malloc( length + 1); // space for name
		p->next = NULL;
		if( p->pname != NULL ) {
			return( strcpy( p->pname, t ) );
		}
	}


	return ("ERROR!");
}

/* ===================================================================================
	functions to extract object information
	==================================================================================
*/


/* gets the data type of object from CPU */
static void PviReadDataType( PVIOBJECT *object ) {
	char buffer[256];
	int result;
	char *s;

	result = PviRead( object->linkid, POBJ_ACC_TYPE_EXTERN, NULL, 0, buffer, sizeof(buffer) );
	if( result == 0 ) {
		s = buffer;

		while ( *s ) {
			if (*s == ' ') {	
				++s;			// ignore spaces
				continue;
			}

			if( FindToken( &s, "{" ) ) { // start of struct definition
				break;
			}

			if (FindToken(&s, KWDESC_PVLEN "=" )) { 	// variable's byte length
				object->ex.pv.length = GetIntValue(&s);
				continue;
			}

			if (FindToken(&s,  KWDESC_PVTYPE "=")) {	// variable type
				if (FindToken(&s, KWPVTYPE_BOOLEAN)) {
					object->ex.pv.type = BR_BOOL;
					object->ex.pv.length = 1;
					object->ex.pv.pdatatype = "BOOL";
				} else if (FindToken(&s, KWPVTYPE_UINT8)) {
					object->ex.pv.type = BR_USINT;
					object->ex.pv.length = 1;
					object->ex.pv.pdatatype = "USINT";
				} else if (FindToken(&s, KWPVTYPE_INT8)) {
					object->ex.pv.type = BR_SINT;
					object->ex.pv.length = 1;
					object->ex.pv.pdatatype = "SINT";
				} else if (FindToken(&s, KWPVTYPE_UINT16)) {
					object->ex.pv.type = BR_UINT;
					object->ex.pv.length = 2;
					object->ex.pv.pdatatype = "UINT";
				} else if (FindToken(&s, KWPVTYPE_INT16)) {
					object->ex.pv.type = BR_INT;
					object->ex.pv.length = 2;
					object->ex.pv.pdatatype = "INT";
				} else if (FindToken(&s, KWPVTYPE_UINT32)) {
					object->ex.pv.type = BR_UDINT;
					object->ex.pv.length = 4;
					object->ex.pv.pdatatype = "UDINT";
				} else if (FindToken(&s, KWPVTYPE_INT32)) {
					object->ex.pv.type = BR_DINT;
					object->ex.pv.length = 4;
					object->ex.pv.pdatatype = "DINT";
				} else if (FindToken(&s, KWPVTYPE_FLOAT32)) {
					object->ex.pv.type = BR_REAL;
					object->ex.pv.length = 4;
					object->ex.pv.pdatatype = "REAL";
				} else if (FindToken(&s, KWPVTYPE_FLOAT64)) {
					object->ex.pv.type = BR_LREAL;
					object->ex.pv.length = 8;
					object->ex.pv.pdatatype = "LREAL";
				} else if (FindToken(&s, KWPVTYPE_DATI)) {
					object->ex.pv.type = BR_DATI;
					object->ex.pv.length = 4;
					object->ex.pv.pdatatype = "DATE_AND_TIME";
				} else if (FindToken(&s, KWPVTYPE_DATE)) {
					object->ex.pv.type = BR_DATE;
					object->ex.pv.length = 4;
					object->ex.pv.pdatatype = "DATE";
				} else if (FindToken(&s, KWPVTYPE_TIME)) {
					object->ex.pv.type = BR_TIME;
					object->ex.pv.length = 4;
					object->ex.pv.pdatatype = "TIME";
				} else if (FindToken(&s, KWPVTYPE_TOD)) {
					object->ex.pv.type = BR_TOD;
					object->ex.pv.length = 4;
					object->ex.pv.pdatatype = "TOD";
				} else if (FindToken(&s, KWPVTYPE_STRUCTURE)) {
					object->ex.pv.type = BR_STRUCT;
					if( object->ex.pv.pdatatype == NULL )
						object->ex.pv.pdatatype = "DATATYPE";
				} else if (FindToken(&s, KWPVTYPE_STRING)) {
					object->ex.pv.type = BR_STRING;
					object->ex.pv.pdatatype = "STRING";
				} else if (FindToken(&s, KWPVTYPE_WSTRING)) {
					object->ex.pv.type = BR_WSTRING;
					object->ex.pv.pdatatype = "WSTRING";
				} else {
					object->ex.pv.type = 0;	
					object->ex.pv.pdatatype = "UNKNOWN";
					while (*s != ' ' && *s != 0)
						++s;	// ignore rest
				}
				continue;
			}

			if (FindToken(&s, KWDESC_SCOPE "=")) {  // scope of variable
				object->ex.pv.scope[0] = *s++;
				object->ex.pv.scope[1] = 0;
				while (*s != ' ' && *s != 0)
					++s;	// ignore rest
				continue;
			}

			if (FindToken(&s, KWDESC_PVCNT "=")) {	// number of elements
				object->ex.pv.dimension = GetIntValue(&s);
				continue;
			}
			++s;
		} /* end while()*/

		if( object->ex.pv.type == BR_STRUCT ) {
			//PviReadStructElements( object );
		}

	}
}


/* creates the children of the object as seperate objects */
PVIOBJECT *ExpandPviObject( PVIOBJECT * object ) {
	int i;
	PVIOBJECT *tempobject;

	if( object == NULL )
		return NULL;

	tempobject = malloc(sizeof(PVIOBJECT));

	if( tempobject != NULL ) {

		if( object->gui_info.has_childs == 0 ) {
			switch( object->type ) {
				case POBJ_PVAR: 	/* process variable */
					if( object->ex.pv.dimension > 1 ) { // array ?

						if( object->ex.pv.type == BR_STRUCT ) { // struct array ?
							PviReadStructElements( object ); // get struct definition only once
						}

						// create all array elements
						for( i = 0; i < object->ex.pv.dimension; ++i ) {
							memcpy( tempobject, object, sizeof(PVIOBJECT) );
							tempobject->ex.pv.dimension = 1;
							snprintf( tempobject->name, sizeof(tempobject->name), "%s[%i]", object->name, i );
							snprintf( tempobject->descriptor, sizeof(tempobject->descriptor), "%s[%i]", object->descriptor, i );
							if( AddPviObject( tempobject, FALSE ) != NULL ) {
								object->gui_info.has_childs = 1;  // at least one child was found
							}
						}

					}

					else

						if( object->ex.pv.type == BR_STRUCT ) {
							if( PviReadStructElements( object ) != NULL ) {
								object->gui_info.has_childs = 1;
							}
						}
					break;

				case POBJ_CPU:
					if( PviReadTaskList(object) != NULL ) {
						object->gui_info.has_childs = 1;
					}
					object->gui_info.has_childs = 0;
					break;

				case POBJ_TASK:
					if( PviReadPvarList(object) == 0 ) {
						object->gui_info.has_childs = 1;
					}
					break;

				case POBJ_DEVICE:
					PviReadCPUList(object);
					object->gui_info.has_childs = 0;
					break;

				default:
					break;
			}
		}

		free(tempobject);
	}
	return NULL;
}



/* searches for reachable CPUs
	input: pointer to device object
	result : pointer to device object or NULL in case of error

*/

static PVIOBJECT *PviReadCPUList(PVIOBJECT *deviceObject ) {
	PVIOBJECT *cpuObject;
	char *s;
	char descriptor[256];
	int result;
	char *pbuffer;

	if( deviceObject == NULL )
		return NULL;

	if( deviceObject->type != POBJ_DEVICE )
		return NULL;


	pbuffer = malloc(65535);
	if( pbuffer == NULL ) {
		AddRowToLog( "PviReadCPUList():malloc(65535)", -1 );
		return NULL;
	}


	cpuObject = calloc( 1, sizeof(PVIOBJECT) );
	if( cpuObject == NULL ) {
		free(pbuffer);
		AddRowToLog( "PviReadCPUList():malloc(PVIOBJECT)", -1 );
		return NULL;
	}


	s= _strupr(deviceObject->descriptor);
	while( *s ) {
		// search for CPUs connected to a serial port
		if( FindToken( &s, "/IF=COM" ) ) {

			CreateUniqueObjectName( tempstring, cpuObject->descriptor );
			snprintf( cpuObject->name, sizeof(cpuObject->name), "%s/CPU%s", deviceObject->name, tempstring  );

			cpuObject->type = POBJ_CPU;
			snprintf( descriptor, sizeof(descriptor), "CD=\"%s\"", cpuObject->descriptor );

			result = PviCreate(&cpuObject->linkid, cpuObject->name, cpuObject->type, descriptor, PviCallback, SET_PVICALLBACK_DATA, 0, "Ev=e" );
			if( result == 0 ) {
				result = PviRead(cpuObject->linkid, POBJ_ACC_VERSION, NULL, 0, pbuffer, 65535 );
				if( result == 0 ) {
					if( AddPviObject( cpuObject, 1 ) == NULL ) {
						deviceObject = NULL; //Fehler
					}
					break;
				}
			}
			switch( result ) {
				case 4808:
					ReportError( REPORT_WARN, "Error", "No CPU found !" );
					break;

				case 13076:
					ReportError( REPORT_WARN, "Error", "Device does not exist !" );
					break;


				default:
					AddRowToLog( "PviReadCPUList()", result );
					break;
			}
			break;
		}

		// search for CPUs connected via ethernet
		if( FindToken( &s, "/IF=TCPIP" ) ) {
			struct stEthernetCpuInfo *buffer = calloc(256, sizeof(struct stEthernetCpuInfo) );
			struct stEthernetCpuInfo *ethernetCpuInfo = buffer;

			if( ethernetCpuInfo != NULL ) {

				int i;
				int noOfCpu = SearchEthernetCpus( ethernetCpuInfo, 256 );
				for( i = 0; i < noOfCpu; ++i, ++ethernetCpuInfo ) {
					memset( cpuObject, 0, sizeof(PVIOBJECT) );
					memcpy( &cpuObject->ex.cpu.ethernetCpuInfo, (void*) ethernetCpuInfo, sizeof(struct stEthernetCpuInfo) );
					sprintf( cpuObject->descriptor, "/DAIP=%s", ethernetCpuInfo->ipAddress );
					strcpy( cpuObject->ex.cpu.arversion, ethernetCpuInfo->arVersion );
					strcpy( cpuObject->ex.cpu.cputype, ethernetCpuInfo->targetTypeDescription );
					if( strlen(cpuObject->ex.cpu.ethernetCpuInfo.macAddress) ) {
						// the MAC is the better unique name
						snprintf( cpuObject->name, sizeof(cpuObject->name), "%s/CPU%s", deviceObject->name, cpuObject->ex.cpu.ethernetCpuInfo.macAddress );
					} else {
						CreateUniqueObjectName( tempstring, cpuObject->descriptor );
						snprintf( cpuObject->name, sizeof(cpuObject->name), "%s/CPU%s", deviceObject->name, tempstring );
					}
					cpuObject->type = POBJ_CPU;
					snprintf( descriptor, sizeof(descriptor), "CD=\"%s\"", cpuObject->descriptor );
					result = PviCreate(&cpuObject->linkid, cpuObject->name, cpuObject->type, descriptor, PviCallback, SET_PVICALLBACK_DATA, 0, "Ev=s" );
					if( result == 0 ) {
						AddPviObject( cpuObject, 1 );
					}
				}
				free(buffer);
			}
			break;
		}

		++s;
	}

	free(pbuffer);
	free(cpuObject);
	return deviceObject;
}


/* Reads a list of tasks available on the CPU */
static PVIOBJECT * PviReadTaskList(PVIOBJECT *object) {
	PVIOBJECT *tempobject, *o;
	int result = 0;
	char *pbuffer;
	char *s, *d;

	if( object == NULL )
		return NULL;

	if( object->type != POBJ_CPU )
		return NULL;

	pbuffer = (char *)malloc(65535);

	if( pbuffer == NULL ) {
		AddRowToLog("PviRefreshTaskList:malloc(65535)", -1 );
		return NULL;  // error
	}

	tempobject = malloc(sizeof(PVIOBJECT));

	if( tempobject == NULL ) { // error
		free(pbuffer); 
		AddRowToLog("PviRefreshTaskList:malloc(PVIOBJECT)", -1 );
		return NULL;  
	}

	/* get version of Automation Runtime  */
	object->ex.cpu.arversion[0] = 0;
	result = PviRead(object->linkid, POBJ_ACC_VERSION, NULL, 0, object->ex.cpu.arversion, sizeof(object->ex.cpu.arversion)-1 );

	if( result != 0 ) {
		return NULL;
	}

	/* get CPU type */
	object->ex.cpu.cputype[0] = 0;
	result = PviRead(object->linkid, POBJ_ACC_CPU_INFO, NULL, 0, pbuffer, 65535);
	if( result == 0 ) {
		s = pbuffer;
		while( s ) {
			if( FindToken( &s, "CT=" ) ) {
				strcpy( object->ex.cpu.cputype, s );
				break;
			}
			++s;
		}
	}

	/* get CPU status */
	result = PviRead(object->linkid, POBJ_ACC_STATUS, NULL, 0, pbuffer, 65535 );
	if( result != 0 ) {
		return NULL;

	}


	/* read list of tasks */
	if( object->error != 0 )
		return NULL;

	free(pbuffer);
	result = PviReadAll(object->linkid, POBJ_ACC_LIST_TASK, &pbuffer);  // list can exceed a fixed buffer
	if (result == 0) {
		s = pbuffer;
		while(*s) {
			memset(tempobject, 0, sizeof(PVIOBJECT));
			d = tempobject->descriptor;
			do {
				*d++ = *s++;
			} while ((*s != 0) && (*s != '\t'));
			if (*s) ++s;			// ignore tabs (but don't step past the end)
			*d = 0;					// terminate with zero
			strcpy( tempobject->name, object->name );
			strcat( tempobject->name, "/" );
			strcat( tempobject->name, tempobject->descriptor );
			tempobject->type = POBJ_TASK;

			if( (o=AddPviObject(tempobject, 1)) != NULL ) {
				o->ex.task.cpu = object; // store pointer to CPU object
				object->gui_info.has_childs = 1; // children were found
			}
		}
	} else {
		AddRowToLog("PviRefreshTaskList", result);
	}


	free(pbuffer);
	free(tempobject);
	return object;  // return pointer to CPU object
}


/* reads the list of variables that are accessible via the specified object */ 
static PVIOBJECT *PviReadPvarList(PVIOBJECT * object) {
	PVIOBJECT newobject;
	char *pbuffer, *s, *d;
	int result;

	if (object == NULL)
		return NULL;

	if (object->type != POBJ_CPU && object->type != POBJ_TASK)
		return NULL;


	/* link object */
	AddPviObject( object, 1 );
	/* read list of variables - can exceed a fixed buffer on large projects */
	result = PviReadAll(object->linkid, POBJ_ACC_LIST_PVAR, &pbuffer);
	if (result == 0) {
		/* append a tab so the last entry is terminated like the others */
		size_t len = strlen(pbuffer);
		char *nbuf = (char *)realloc(pbuffer, len + 2);
		if (nbuf == NULL) {
			free(pbuffer);
			AddRowToLog("PviReadPvarList:realloc", -1);
			return NULL;
		}
		pbuffer = nbuf;
		pbuffer[len] = '\t';
		pbuffer[len + 1] = 0;
		s = pbuffer;

		memset(&newobject, 0, sizeof(PVIOBJECT));
		while ( *s ) {
			if (*s == ' ') {	
				++s;			// ignore space character
				continue;
			}

			if (strlen(newobject.descriptor) == 0) {	// get variable name
				d = newobject.descriptor;
				while (*s && *s != ' ') {
					*d++ = *s++;
				}
				*d = 0;			// terminate with zero;
				continue;
			}

			if (FindToken(&s, KWDESC_PVLEN "=" )) { 	// variable's byte length
				newobject.ex.pv.length = GetIntValue(&s);
				continue;
			}

			if (FindToken(&s,  KWDESC_PVTYPE "=")) {	// variable type
				if (FindToken(&s, KWPVTYPE_BOOLEAN)) {
					newobject.ex.pv.type = BR_BOOL;
					newobject.ex.pv.length = 1;
					newobject.ex.pv.pdatatype = "BOOL";
				} else if (FindToken(&s, KWPVTYPE_UINT8)) {
					newobject.ex.pv.type = BR_USINT;
					newobject.ex.pv.length = 1;
					newobject.ex.pv.pdatatype = "USINT";
				} else if (FindToken(&s, KWPVTYPE_INT8)) {
					newobject.ex.pv.type = BR_SINT;
					newobject.ex.pv.length = 1;
					newobject.ex.pv.pdatatype = "SINT";
				} else if (FindToken(&s, KWPVTYPE_UINT16)) {
					newobject.ex.pv.type = BR_UINT;
					newobject.ex.pv.length = 2;
					newobject.ex.pv.pdatatype = "UINT";
				} else if (FindToken(&s, KWPVTYPE_INT16)) {
					newobject.ex.pv.type = BR_INT;
					newobject.ex.pv.length = 2;
					newobject.ex.pv.pdatatype = "INT";
				} else if (FindToken(&s, KWPVTYPE_UINT32)) {
					newobject.ex.pv.type = BR_UDINT;
					newobject.ex.pv.length = 4;
					newobject.ex.pv.pdatatype = "UDINT";
				} else if (FindToken(&s, KWPVTYPE_INT32)) {
					newobject.ex.pv.type = BR_DINT;
					newobject.ex.pv.length = 4;
					newobject.ex.pv.pdatatype = "DINT";
				} else if (FindToken(&s, KWPVTYPE_FLOAT32)) {
					newobject.ex.pv.type = BR_REAL;
					newobject.ex.pv.length = 4;
					newobject.ex.pv.pdatatype = "REAL";
				} else if (FindToken(&s, KWPVTYPE_FLOAT64)) {
					newobject.ex.pv.type = BR_LREAL;
					newobject.ex.pv.length = 4;
					newobject.ex.pv.pdatatype = "LREAL";
				} else if (FindToken(&s, KWPVTYPE_DATI)) {
					newobject.ex.pv.type = BR_DATI;
					newobject.ex.pv.length = 4;
					newobject.ex.pv.pdatatype = "DATE_AND_TIME";
				} else if (FindToken(&s, KWPVTYPE_DATE)) {
					newobject.ex.pv.type = BR_DATE;
					newobject.ex.pv.length = 4;
					newobject.ex.pv.pdatatype = "DATE";
				} else if (FindToken(&s, KWPVTYPE_TOD)) {
					newobject.ex.pv.type = BR_TOD;
					newobject.ex.pv.length = 4;
					newobject.ex.pv.pdatatype = "TOD";
				} else if (FindToken(&s, KWPVTYPE_TIME)) {
					newobject.ex.pv.type = BR_TIME;
					newobject.ex.pv.length = 4;
					newobject.ex.pv.pdatatype = "TIME";
				} else if (FindToken(&s, KWPVTYPE_STRUCTURE)) {
					newobject.ex.pv.type = BR_STRUCT;
					if( newobject.ex.pv.pdatatype == NULL )
						newobject.ex.pv.pdatatype = "DATATYPE";
				} else if (FindToken(&s, KWPVTYPE_STRING)) {
					newobject.ex.pv.type = BR_STRING;
					newobject.ex.pv.pdatatype = "STRING";
				} else if (FindToken(&s, KWPVTYPE_WSTRING)) {
					newobject.ex.pv.type = BR_WSTRING;
					newobject.ex.pv.pdatatype = "WSTRING";
				} else {
					newobject.ex.pv.type = 0;	// unknown
					newobject.ex.pv.pdatatype = "UNKNOWN";
					while (*s != ' ' && *s != 0)
						++s;	// ignore rest
				}
				continue;
			}

			if (FindToken(&s, KWDESC_SCOPE "=")) {  // variable scope
				newobject.ex.pv.scope[0] = *s++;
				newobject.ex.pv.scope[1] = 0;
				while (*s != ' ' && *s != 0)
					++s;	// ignore rest
				continue;
			}

			if (FindToken(&s, KWDESC_PVCNT "=")) {	// number of elements
				newobject.ex.pv.dimension = GetIntValue(&s);
				continue;
			}

			if (FindToken( &s, "\t" ) || *(s+1) == 0 ) {	// end of entry or of complete list
				newobject.type = POBJ_PVAR;
				strcpy( newobject.name, object->name );
				strcat( newobject.name, "/" );
				strcat( newobject.name, newobject.descriptor );
				if( object->type == POBJ_CPU ) { // global variable ?
					newobject.ex.pv.task = object;  // since task does not exist we store CPU object instead
					newobject.ex.pv.cpu = object;	// store CPU object
				} else { // local variable
					PVIOBJECT *cpu = object->ex.task.cpu;

					newobject.ex.pv.task = object; // store pointer to task object
					newobject.ex.pv.cpu = cpu;  // store pointer to CPU object
				}
				if( AddPviObject(&newobject, 0) != NULL ) {
					object->gui_info.has_childs = 1; // children were created as separate objects
				}
				memset(&newobject, 0, sizeof(PVIOBJECT));
				continue;
			}

			++s;
		}						/* end of while (*s != 0)... */

	} else {
		AddRowToLog("PviRefreshTaskList(Variablen)", result);
	}

	//Speicher freigeben
	free(pbuffer);
	return (object);

}



/* PviRead a string-valued access type of unknown length. PVI silently
   truncates to the buffer size, so retry with a doubled buffer while the
   result fills it completely. On success *out is a NUL-terminated malloc'd
   buffer the caller frees; on error *out is NULL. Returns the PVI result,
   or -1 if out of memory. */
#define PVI_READ_ALL_INITIAL (64 * 1024)
#define PVI_READ_ALL_MAX     (64 * 1024 * 1024)
static int PviReadAll( DWORD linkid, int access, char **out ) {
	size_t size = PVI_READ_ALL_INITIAL;
	char *buf = NULL;
	int result;

	*out = NULL;
	for( ;; ) {
		char *nbuf = (char *)realloc( buf, size + 1 );
		if( nbuf == NULL ) {
			free( buf );
			return -1;
		}
		buf = nbuf;
		result = PviRead( linkid, access, NULL, 0, buf, (LONG)size );
		if( result != 0 ) {
			free( buf );
			return result;
		}
		if( strnlen( buf, size ) < size || size >= PVI_READ_ALL_MAX )
			break;
		size *= 2;
	}
	buf[size] = 0;
	*out = buf;
	return 0;
}

/* read struct elements and use them as separate objects */
static PVIOBJECT *PviReadStructElements( PVIOBJECT *object ) {
	char *pbuffer;
	int result;
	char *s, *d;
	PVIOBJECT newobject;
	PVIOBJECT *o;
	BOOL element_definition;

	if( object == NULL )
		return NULL;

	if( object->type != POBJ_PVAR && object->ex.pv.type != BR_STRUCT )
		return NULL;

	pbuffer = NULL;

	memcpy( &newobject, object, sizeof(PVIOBJECT) );
	strcpy( newobject.descriptor, "CD=\"" );
	strcat( newobject.descriptor, object->descriptor );
	strcat( newobject.descriptor, "\"" );

	// The extern type lists every nested member, so large structs easily
	// exceed a fixed buffer - read it whole.
	result = PviCreate(&newobject.linkid, newobject.name, newobject.type, newobject.descriptor, PviCallback, SET_PVICALLBACK_DATA, 0, "Ev=e");
	if( result == 0 || result == 12002 ) {
		result = PviReadAll( newobject.linkid, POBJ_ACC_TYPE_EXTERN, &pbuffer );
	}
	PviDelete( newobject.name );

	if( result == 0 ) {
		//AddRowToLog( pbuffer, 0 );

		s = pbuffer;
		element_definition = FALSE;

		while( *s != 0 ) {
			if( *s == ' ' ) {  
				++s;		//ignore space character
				continue;
			}

			if( !element_definition  ) { // Strukturname des Parents
				if( FindToken(&s, KWDESC_SNAME "=" ) ) {
					d = tempstring;
					while( *s && *s != ' ' ) {
						*d++=*s++;
					}
					*d = 0;
					object->ex.pv.pdatatype = AddPviDatatype(tempstring);
				}
			}

			if( FindToken( &s, "{" ) ) {  // start of element definition
				element_definition = TRUE;
				memset( &newobject, 0, sizeof(PVIOBJECT) );
				memset( tempstring, 0, sizeof(tempstring) );
				d = tempstring;
				while( *s && *s == ' ' ) // ignore space character
					++s;
				while( *s && *s!=' ' )
					*d++ = *s++;
				*d = 0;
				strcpy( newobject.descriptor, object->descriptor );  // create descriptor
				strcat( newobject.descriptor, tempstring );
				strcpy( newobject.name, object->name );		// create name
				strcat( newobject.name, tempstring );
				newobject.type =POBJ_PVAR;
				// count number of points. If > then element is a child
				if( CountToken( tempstring, '.' ) > 1 ) {
					while( !FindToken( &s, "}" ) )
						++s;  // ignore everything
					element_definition = FALSE;
				}
				continue;
			}

			else

				if( element_definition == FALSE ) {
					++s;
					continue;
				}



			if (FindToken(&s, KWDESC_PVLEN "=" )) { 	// variable byte length
				newobject.ex.pv.length = GetIntValue(&s);
				continue;
			}

			if (FindToken(&s, KWDESC_PVCNT "=" )) { 	// number of elements
				newobject.ex.pv.dimension = GetIntValue(&s);
				continue;
			}



			if (FindToken(&s,  KWDESC_PVTYPE "=")) {	// variable type
				if (FindToken(&s, KWPVTYPE_BOOLEAN)) {
					newobject.ex.pv.type = BR_BOOL;
					newobject.ex.pv.length = 1;
					newobject.ex.pv.pdatatype = "BOOL";
				} else if (FindToken(&s, KWPVTYPE_UINT8)) {
					newobject.ex.pv.type = BR_USINT;
					newobject.ex.pv.length = 1;
					newobject.ex.pv.pdatatype = "USINT";
				} else if (FindToken(&s, KWPVTYPE_INT8)) {
					newobject.ex.pv.type = BR_SINT;
					newobject.ex.pv.length = 1;
					newobject.ex.pv.pdatatype = "SINT";
				} else if (FindToken(&s, KWPVTYPE_UINT16)) {
					newobject.ex.pv.type = BR_UINT;
					newobject.ex.pv.length = 2;
					newobject.ex.pv.pdatatype = "UINT";
				} else if (FindToken(&s, KWPVTYPE_INT16)) {
					newobject.ex.pv.type = BR_INT;
					newobject.ex.pv.length = 2;
					newobject.ex.pv.pdatatype = "INT";
				} else if (FindToken(&s, KWPVTYPE_UINT32)) {
					newobject.ex.pv.type = BR_UDINT;
					newobject.ex.pv.length = 4;
					newobject.ex.pv.pdatatype = "UDINT";
				} else if (FindToken(&s, KWPVTYPE_INT32)) {
					newobject.ex.pv.type = BR_DINT;
					newobject.ex.pv.length = 4;
					newobject.ex.pv.pdatatype = "DINT";
				} else if (FindToken(&s, KWPVTYPE_FLOAT32)) {
					newobject.ex.pv.type = BR_REAL;
					newobject.ex.pv.length = 4;
					newobject.ex.pv.pdatatype = "REAL";
				} else if (FindToken(&s, KWPVTYPE_FLOAT64)) {
					newobject.ex.pv.type = BR_LREAL;
					newobject.ex.pv.length = 8;
					newobject.ex.pv.pdatatype = "LREAL";
				} else if (FindToken(&s, KWPVTYPE_DATI)) {
					newobject.ex.pv.type = BR_DATI;
					newobject.ex.pv.length = 4;
					newobject.ex.pv.pdatatype = "DATE_AND_TIME";
				} else if (FindToken(&s, KWPVTYPE_DATE)) {
					newobject.ex.pv.type = BR_DATI;
					newobject.ex.pv.length = 4;
					newobject.ex.pv.pdatatype = "DATE";
				} else if (FindToken(&s, KWPVTYPE_TOD)) {
					newobject.ex.pv.type = BR_TOD;
					newobject.ex.pv.length = 4;
					newobject.ex.pv.pdatatype = "TOD";
				} else if (FindToken(&s, KWPVTYPE_TIME)) {
					newobject.ex.pv.type = BR_TIME;
					newobject.ex.pv.length = 4;
					newobject.ex.pv.pdatatype = "TIME";
				} else if (FindToken(&s, KWPVTYPE_STRUCTURE)) {
					newobject.ex.pv.type = BR_STRUCT;
					if( newobject.ex.pv.pdatatype == NULL )
						newobject.ex.pv.pdatatype = "DATATYPE(STRUCT)";
				} else if (FindToken(&s, KWPVTYPE_STRING)) {
					newobject.ex.pv.type = BR_STRING;
					newobject.ex.pv.pdatatype = "STRING";
				} else if (FindToken(&s, KWPVTYPE_WSTRING)) {
					newobject.ex.pv.type = BR_WSTRING;
					newobject.ex.pv.pdatatype = "WSTRING";
				} else {
					newobject.ex.pv.type = 0;	// unbekannt
					newobject.ex.pv.pdatatype = "UNKNOWN";
					while (*s != ' ' && *s != 0)
						++s;	// Rest ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¼berlesen
				}
				continue;
			}

			if( element_definition  && !strcmp(newobject.ex.pv.pdatatype, "DATATYPE") ) { // Strukturname des Elementes
				if( FindToken(&s, KWDESC_SNAME "=" ) ) {
					d = tempstring;
					while( *s && *s != ' ' ) {
						*d++=*s++;
					}
					*d = 0;
					newobject.ex.pv.pdatatype = AddPviDatatype(tempstring);
				}
			}
			if (FindToken(&s, KWDESC_PVCNT "=")) {	// Anzahl der Elemente
				newobject.ex.pv.dimension = GetIntValue(&s);
				continue;
			}

			if (FindToken( &s, "}" ) || *(s+1) == 0 ) {	// Ende des Eintrags oder der gesamten Liste
				PVIOBJECT newelement;
				int i;
//				int j;
				element_definition = FALSE;

				for( i = 0; i < object->ex.pv.dimension; ++i ) {
#ifdef RAUS
					for( j = 0; j < newobject.ex.pv.dimension; ++j ) {
#endif
						memcpy( &newelement, &newobject, sizeof(PVIOBJECT) );
#ifdef RAUS
						newelement.ex.pv.dimension = 1;  // Dimension wird nicht geerbt
#endif

						if( object->ex.pv.dimension > 1 ) { // Elternobject ist ein Array
							strcpy( tempstring, newobject.descriptor+strlen(object->descriptor)+1 ); // Name des Elementes
							snprintf( newelement.name, sizeof(newelement.name), "%s[%i].%s", object->name, i, tempstring );
							snprintf( newelement.descriptor, sizeof(newelement.descriptor), "%s[%i].%s", object->descriptor, i, tempstring );
						}

#ifdef RAUS
						if( newobject.ex.pv.dimension > 1 ) { // Kindobject ist ein Array
							snprintf( tempstring, sizeof(tempstring), "[%i]", j );
							strcat( newelement.name, tempstring );
							strcat( newelement.descriptor, tempstring );
						}
#endif

						strncpy( newelement.ex.pv.scope, object->ex.pv.scope, 1 ); // GÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¼ltigkeitsbereich wird geerbt
						newelement.ex.pv.task = object->ex.pv.task;  // Task wird geerbt
						newelement.ex.pv.cpu = object->ex.pv.cpu;  // Task wird geerbt
						if( (o = AddPviObject( &newelement, FALSE )) != NULL ) {
							//object->gui_info.has_childs = 1;  // mind. 1 "Kind" wurde fÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¼r das Objekt angelegt
							//
							//if( o->ex.pv.type == BR_STRUCT ){
							//	o->gui_info.has_childs = 1; // Strukturen haben mind. 1 Kind
							//}

							//sprintf( tempstring, "%s[%i]", object->name, i );
							//if( (o = FindPviObjectByName( tempstring )) != NULL ){
							//	o->gui_info.has_childs = 1;  // Arrayelemente haben mind. 1 "Kind"
							//}

						}
#ifdef RAUS
					} // for( j = 0...
#endif
				} // for (i = 0...
				continue;
			}
			++s;
		}
	}


	if( result ) {
		AddRowToLog( "PviReadStructElements:PviCreate/PviRead", result );
		object = NULL;
	}

	free(pbuffer);
	return object;
}






/* ======================================================================================
   log file handling
   ====================================================================================== */


/* writes into log file */
static void AddRowToLog(char *row, int errcode) {
	FILE *f;
	char zeit[10];
	char datum[10];
	char filename[MAX_PATH];

	strcpy( filename, GetApplicationPath() );
	strcat( filename, "\\" PVILOGFILE );

	_strtime(zeit);
	_strdate(datum);

	f = fopen(filename, "a+");

	if (f != NULL) {
		if (errcode != 0) {
			fprintf(f, "%s - %s : %s --> Error:%i\n", datum, zeit, row, errcode);
		} else {
			fprintf(f, "%s - %s : %s --> OK! \n", datum, zeit, row);
		}
		fclose(f);
	}

	if (errcode != 0) {
		char *errtext;
		switch( errcode ) {
			case 12050:
				errtext = "PVI-Manager not startet";
				break;
			case 12060:
				errtext = "process timeout";
				break;
			default:
				errtext = "please refer to PVI documentation";
		}
		ReportError( REPORT_ERROR, "Error", "Error in PVI-Interface\nError: %u\n\"%s\"\n(%s)", errcode, errtext, row );
	}

}


static void DeleteLog(void) {
	char filename[MAX_PATH];

	strcpy( filename, GetApplicationPath() );
	strcat( filename, "\\" PVILOGFILE );
	remove( filename );
}

/* =====================================================================================
	watch file handling
	====================================================================================
*/


/* writes object to watch file */
int AddToPviWatchFile(PVIOBJECT * object, char *filename) {
	PVIOBJECT *parent;
	char *section;
	int entries;
//	int result;
	int i;



	// store file version info
	WritePrivateProfileString( "File", "Application", "BRWATCH.EXE", filename ); // type
	WritePrivateProfileString( "File", "Version", WATCH_FILE_VERSION, filename ); // version

	parent = GetNextPviObject(TRUE);

	// Eltern suchen und ggfs. speichern
	while (parent != NULL) {
		if ( IsPviObjectParentOf(parent, object) && strrchr(parent->name, '.') == NULL ) {	// find parents
			switch (parent->type) {
				case POBJ_DEVICE:
					section = KWOBJTYPE_DEVICE;
					break;
				case POBJ_CPU:
					section = KWOBJTYPE_CPU;
					break;
				case POBJ_TASK:
					section = KWOBJTYPE_TASK;
					break;
				case POBJ_PVAR:
					section = KWOBJTYPE_PVAR;
					break;
				default:
					section = NULL;
					break;
			}
			if (section != NULL) {
				char keyname[80];
				BOOL found = FALSE;
				tempstring[0] = 0;
				GetPrivateProfileString( section, section, "0", tempstring, sizeof(tempstring), filename );
				entries = atoi(tempstring);
				// does entry exist ?
				for( i = 1; i <= entries; ++i ) {
					sprintf( keyname, "name%u", i );
					GetPrivateProfileString( section, keyname, "", tempstring, sizeof(tempstring), filename );
					if( strcmp( parent->name, tempstring ) == 0 ) {
						found = TRUE;
					}
				}

				if( !found ) { // not found
					++entries;
					sprintf( tempstring, "%u", entries );
					WritePrivateProfileString( section, section, tempstring, filename );
					// Eltern-Objekt speichern
					sprintf( keyname, "name%u", entries );
					WritePrivateProfileString( section, keyname, parent->name, filename ); // name
					sprintf( keyname, "desc%u", entries );
					WritePrivateProfileString( section, keyname, parent->descriptor, filename ); // descriptor
					sprintf( keyname, "sort%u", entries );
					WritePrivateProfileString( section, keyname, "-1", filename ); // sort criteria
				}
			}
		}
		parent = GetNextPviObject(FALSE);
	}

	// Objekt und Sortierkritierium speichern
	switch (object->type) {
		case POBJ_CPU:
			section = KWOBJTYPE_CPU;
			break;
		case POBJ_TASK:
			section = KWOBJTYPE_TASK;
			break;
		case POBJ_PVAR:
			section = KWOBJTYPE_PVAR;
			break;
		default:
			section = NULL;
			break;
	}
	if( section != NULL ) {
		char keyname[80];
		tempstring[0] = 0;
		// number of entries
		GetPrivateProfileString( section, section, "0", tempstring, sizeof(tempstring), filename );
		entries = atoi(tempstring);
		++entries;
		sprintf( tempstring, "%u", entries );
		WritePrivateProfileString( section, section, tempstring, filename );
		// Objekt speichern
		sprintf( keyname, "name%u", entries );
		WritePrivateProfileString( section, keyname, object->name, filename ); // name
		sprintf( keyname, "desc%u", entries );
		WritePrivateProfileString( section, keyname, object->descriptor, filename ); // descriptor
		sprintf( keyname, "sort%u", entries );
		sprintf( tempstring, "%u", object->watchsort );
		WritePrivateProfileString( section, keyname, tempstring, filename ); // sort criteria
	}



	return 0;

}

 
/* load objects of filtered type from watch file 
and returns number of objects read */ 
int LoadPviObjectsFromWatchFile(T_POBJ_TYPE typefilter, char *filename) {
	char *section="";
	PVIOBJECT tempobject, *object, *parent;
	int entries, i, sort;
	int n = 0;  // number of objects to be inserted

	switch( typefilter ) {
		case POBJ_DEVICE:
			section = KWOBJTYPE_DEVICE;
			break;
		case POBJ_CPU:
			section = KWOBJTYPE_CPU;
			break;
		case POBJ_TASK:
			section = KWOBJTYPE_TASK;
			break;
		case POBJ_PVAR:
			section = KWOBJTYPE_PVAR;
			break;

		default:
			break;
	}

	// read number of objects in this section
	tempstring[0] = 0;
	GetPrivateProfileString(section, section, "0", tempstring, sizeof(tempstring), filename);
	entries = atoi(tempstring);
	if (entries == 0) {
		ReportError( REPORT_ERROR, "Error Loading Watch File", "No entries found in section [%s] !", section );
		return -1;
	}
	for (i = 1; i <= entries; ++i) {
		char keyname[80];

		memset(&tempobject, 0, sizeof(PVIOBJECT));
		// read name
		sprintf(keyname, "name%u", i);
		GetPrivateProfileString(section, keyname, "", tempobject.name, sizeof(tempobject.name), filename);
		if (strlen(tempobject.name) == 0) {
			ReportError( REPORT_ERROR, "Error Loading Watch File", "section [%s]\n%s=\ninvalid or not found !", section, keyname );
			return -1;
		}
		// read descriptor
		sprintf(keyname, "desc%u", i);
		GetPrivateProfileString(section, keyname, "", tempobject.descriptor, sizeof(tempobject.descriptor), filename);
		if (strlen(tempobject.descriptor) == 0) {
			ReportError( REPORT_ERROR, "Error Loading Watch File", "section [%s]\n%s=\ninvalid or not found !", section, keyname );
			return -1;
		}
		// read sort criteria
		sprintf(keyname, "sort%u", i);
		GetPrivateProfileString(section, keyname, "-1", tempstring, sizeof(tempstring), filename);
		sort = atoi(tempstring);

		// add object
		tempobject.type = typefilter;
		object = AddPviObject(&tempobject, 1);

		if( object != NULL ) {
			object->gui_info.loaded_from_file = 1;
			object->watchsort = sort;
			if( object->watchsort >= 0 )
				++n;
			PviReadDataType( object );

		}


		// find the corresponding task and CPU
		if( object->type == POBJ_PVAR ) {

			parent = GetNextPviObject(TRUE);
			while (parent != NULL) {
				if ( IsPviObjectParentOf(parent, object) && parent->type == POBJ_CPU ) {	// find parents
					object->ex.pv.cpu = parent;
				}
				parent = GetNextPviObject(FALSE);
			}

			parent = GetNextPviObject(TRUE);
			while (parent != NULL) {
				if ( IsPviObjectParentOf(parent, object) && parent->type == POBJ_TASK ) {	// find parents
					object->ex.pv.task = parent;
				}
				parent = GetNextPviObject(FALSE);
			}


		}


		// find the corresponding CPU
		if( object->type == POBJ_TASK ) {

			parent = GetNextPviObject(TRUE);
			while (parent != NULL) {
				if ( IsPviObjectParentOf(parent, object) && parent->type == POBJ_CPU ) {	// find parents
					object->ex.task.cpu = parent;
				}
				parent = GetNextPviObject(FALSE);
			}
		}


	}
	return n;
}


/* CLI helpers for building watch-list entries by descriptor path.
   Supports dotted struct-member paths like "gSim.Time.Time", including
   indexed arrays of structs like "gMain.Capping[0].Out", by walking each
   level with PviReadStructElements. The leaf must be a scalar (not struct,
   not array) to match the GUI's watchlist rules. */

/* Lists all PVAR descriptors matching a parent (cpu for globals, task for
   locals, or any PVAR whose own descriptor is `parent_desc` for struct
   children). Writes up to buflen bytes to buf as comma-separated. Returns
   the number of matches found (not the number printed). */
static int ListPvarSiblings( PVIOBJECT *cpu, PVIOBJECT *task_or_null,
                              const char *parent_desc, char *buf, int buflen ) {
	PVIOBJECT *o;
	int n = 0, used = 0;
	size_t parent_len = parent_desc ? strlen(parent_desc) : 0;
	if( buflen > 0 ) buf[0] = 0;
	o = GetNextPviObject(TRUE);
	while( (o = GetNextPviObject(FALSE)) != NULL ) {
		if( o->type != POBJ_PVAR ) continue;
		if( o->ex.pv.cpu != cpu ) continue;
		if( task_or_null && o->ex.pv.task != task_or_null ) continue;
		if( !task_or_null && o->ex.pv.task != cpu ) continue;
		if( parent_desc && parent_len > 0 ) {
			/* struct child: descriptor starts with parent_desc + "." and has no further dots */
			if( strncmp(o->descriptor, parent_desc, parent_len) != 0 ) continue;
			if( o->descriptor[parent_len] != '.' ) continue;
			if( strchr(o->descriptor + parent_len + 1, '.' ) != NULL ) continue;
		} else {
			/* top-level: descriptor must have no dots */
			if( strchr(o->descriptor, '.') != NULL ) continue;
		}
		++n;
		if( used + (int)strlen(o->descriptor) + 2 < buflen ) {
			if( used > 0 ) { buf[used++] = ','; buf[used++] = ' '; }
			strcpy( buf + used, o->descriptor );
			used += (int)strlen(o->descriptor);
		}
	}
	return n;
}

/* Walks a dotted suffix starting from `first`, calling PviReadStructElements
   at each level and matching children by full descriptor. On error, reports
   with ReportError and returns NULL. */
static PVIOBJECT *TraverseStructMembers( PVIOBJECT *cpu, PVIOBJECT *first,
                                          const char *suffix ) {
	PVIOBJECT *current = first;
	char path[512];
	const char *p = suffix;

	strncpy( path, first->descriptor, sizeof(path)-1 );
	path[sizeof(path)-1] = 0;

	while( p && *p ) {
		const char *dot;
		size_t seglen, cur_len;
		PVIOBJECT *child = NULL, *o;

		if( current->ex.pv.type != BR_STRUCT ) {
			ReportError( REPORT_ERROR, "AddPvar",
				"'%s' is not a struct (type %d); cannot access member '.%s'",
				path, (int)current->ex.pv.type, p );
			return NULL;
		}

		PviReadStructElements( current );

		dot = strchr( p, '.' );
		seglen = dot ? (size_t)(dot - p) : strlen(p);
		cur_len = strlen(path);
		if( cur_len + 1 + seglen >= sizeof(path) ) {
			ReportError( REPORT_ERROR, "AddPvar", "descriptor path too long" );
			return NULL;
		}
		path[cur_len] = '.';
		memcpy( path + cur_len + 1, p, seglen );
		path[cur_len + 1 + seglen] = 0;

		/* An indexed segment ("Capping[0]") has no object of its own: match
		   the array member by its base name. Reading the array's elements at
		   the next level creates children named "Capping[i].x", so `path`
		   keeps the index. */
		{
			char *bracket = strchr( path + cur_len + 1, '[' );
			size_t match_len = bracket ? (size_t)(bracket - path) : strlen(path);
			long index = bracket ? strtol( bracket + 1, NULL, 10 ) : 0;

			o = GetNextPviObject(TRUE);
			while( (o = GetNextPviObject(FALSE)) != NULL ) {
				if( o->type == POBJ_PVAR
				    && o->ex.pv.cpu == cpu
				    && strlen(o->descriptor) == match_len
				    && strncmp(o->descriptor, path, match_len) == 0 )
				{
					child = o;
					break;
				}
			}
			if( child && bracket ) {
				if( child->ex.pv.dimension <= 1 ) {
					ReportError( REPORT_ERROR, "AddPvar", "'%s' is not an array", child->descriptor );
					return NULL;
				}
				if( index < 0 || index >= (long)child->ex.pv.dimension ) {
					ReportError( REPORT_ERROR, "AddPvar", "'%s' index out of range (array has %d elements)",
						path, (int)child->ex.pv.dimension );
					return NULL;
				}
				if( !dot ) {
					ReportError( REPORT_ERROR, "AddPvar",
						"'%s' is an array element; watching array elements directly is not supported, "
						"specify a member like '%s.someField'", path, path );
					return NULL;
				}
			}
		}
		if( !child ) {
			char siblings[512];
			char parent[512];
			/* parent is path minus this segment - unlike current->descriptor it
			   keeps any array index ("Capping[0]"), which children are named under */
			memcpy( parent, path, cur_len );
			parent[cur_len] = 0;
			ListPvarSiblings( cpu, NULL, parent, siblings, sizeof(siblings) );
			ReportError( REPORT_ERROR, "AddPvar",
				"Struct member '%s' not found. Siblings under '%s': %s",
				path, parent,
				siblings[0] ? siblings : "(none - struct empty or not browseable)" );
			return NULL;
		}
		current = child;
		p = dot ? dot + 1 : NULL;
	}
	return current;
}

/* Validate the final leaf — must be a scalar, not a struct, not an array. */
static int ValidatePvarLeaf( PVIOBJECT *leaf, const char *user_input ) {
	if( leaf->ex.pv.type == BR_STRUCT ) {
		ReportError( REPORT_ERROR, "AddPvar",
			"'%s' is a struct; specify a leaf member like '%s.someField'",
			user_input, user_input );
		return -1;
	}
	if( leaf->ex.pv.dimension > 1 ) {
		ReportError( REPORT_ERROR, "AddPvar",
			"'%s' is an array (%d elements); watching arrays is not supported",
			user_input, leaf->ex.pv.dimension );
		return -1;
	}
	return 0;
}


int AddGlobalPvarByCpuDescriptor( const char *cpu_descriptor, const char *var_name ) {
	PVIOBJECT *obj;
	PVIOBJECT *cpu = NULL;
	PVIOBJECT *leaf = NULL;
	char first_seg[256];
	const char *rest = NULL;
	const char *dot;
	size_t seglen;

	if( !cpu_descriptor || !var_name || !*cpu_descriptor || !*var_name ) return -1;

	/* find the CPU object by its descriptor (set from the [CPUn] INI section) */
	obj = GetNextPviObject(TRUE);
	while( (obj = GetNextPviObject(FALSE)) != NULL ) {
		if( obj->type == POBJ_CPU && strcmp(obj->descriptor, cpu_descriptor) == 0 ) {
			cpu = obj;
			break;
		}
	}
	if( cpu == NULL ) {
		ReportError( REPORT_ERROR, "AddPvar",
			"CPU with descriptor '%s' not found. Check brwatch.ini [CPUn] descriptor matches.",
			cpu_descriptor );
		return -1;
	}

	/* split var_name on first '.' into first_seg (top-level global name) + rest */
	dot = strchr( var_name, '.' );
	seglen = dot ? (size_t)(dot - var_name) : strlen(var_name);
	if( seglen == 0 || seglen >= sizeof(first_seg) ) {
		ReportError( REPORT_ERROR, "AddPvar", "invalid var name '%s'", var_name );
		return -1;
	}
	memcpy( first_seg, var_name, seglen );
	first_seg[seglen] = 0;
	rest = dot ? dot + 1 : NULL;

	/* enumerate the CPU's top-level globals (synchronous via PviRead) */
	PviReadPvarList( cpu );

	/* find the top-level global matching first_seg. PviReadPvarList sets
	   ex.pv.task = cpu for globals (see line ~1451), so match on that. */
	obj = GetNextPviObject(TRUE);
	while( (obj = GetNextPviObject(FALSE)) != NULL ) {
		if( obj->type == POBJ_PVAR
		    && obj->ex.pv.cpu == cpu
		    && (obj->ex.pv.task == cpu || obj->ex.pv.task == NULL)
		    && strcmp(obj->descriptor, first_seg) == 0
		    && (obj->ex.pv.scope[0] == 'g' || obj->ex.pv.scope[0] == 0) )
		{
			leaf = obj;
			break;
		}
	}
	if( leaf == NULL ) {
		char siblings[512];
		ListPvarSiblings( cpu, NULL, NULL, siblings, sizeof(siblings) );
		ReportError( REPORT_ERROR, "AddPvar",
			"Global PVAR '%s' not found under CPU '%s'. Available globals: %s",
			first_seg, cpu_descriptor,
			siblings[0] ? siblings : "(none - PLC refused browse, check ANSL user rights)" );
		return -1;
	}

	/* if the user supplied a dotted path, walk struct members */
	if( rest ) {
		leaf = TraverseStructMembers( cpu, leaf, rest );
		if( leaf == NULL ) return -1;
	}

	if( ValidatePvarLeaf( leaf, var_name ) != 0 ) return -1;

	leaf->watchsort = 0;
	WatchPviObject( leaf, TRUE );
	return 0;
}


int AddLocalPvarByCpuTaskDescriptor( const char *cpu_descriptor, const char *task_descriptor, const char *var_name ) {
	PVIOBJECT *obj;
	PVIOBJECT *cpu = NULL;
	PVIOBJECT *task = NULL;
	PVIOBJECT *leaf = NULL;
	char first_seg[256];
	const char *rest = NULL;
	const char *dot;
	size_t seglen;

	if( !cpu_descriptor || !task_descriptor || !var_name ) return -1;

	/* find the CPU object by descriptor */
	obj = GetNextPviObject(TRUE);
	while( (obj = GetNextPviObject(FALSE)) != NULL ) {
		if( obj->type == POBJ_CPU && strcmp(obj->descriptor, cpu_descriptor) == 0 ) {
			cpu = obj;
			break;
		}
	}
	if( cpu == NULL ) {
		ReportError( REPORT_ERROR, "AddPvar", "CPU with descriptor '%s' not found", cpu_descriptor );
		return -1;
	}

	/* enumerate tasks under this CPU */
	PviReadTaskList( cpu );

	/* find the task by descriptor */
	obj = GetNextPviObject(TRUE);
	while( (obj = GetNextPviObject(FALSE)) != NULL ) {
		if( obj->type == POBJ_TASK && obj->ex.task.cpu == cpu
		    && strcmp(obj->descriptor, task_descriptor) == 0 ) {
			task = obj;
			break;
		}
	}
	if( task == NULL ) {
		ReportError( REPORT_ERROR, "AddPvar", "Task '%s' not found under CPU '%s'", task_descriptor, cpu_descriptor );
		return -1;
	}

	/* split var_name on first '.' */
	dot = strchr( var_name, '.' );
	seglen = dot ? (size_t)(dot - var_name) : strlen(var_name);
	if( seglen == 0 || seglen >= sizeof(first_seg) ) {
		ReportError( REPORT_ERROR, "AddPvar", "invalid var name '%s'", var_name );
		return -1;
	}
	memcpy( first_seg, var_name, seglen );
	first_seg[seglen] = 0;
	rest = dot ? dot + 1 : NULL;

	/* enumerate PVARs under this task */
	PviReadPvarList( task );

	/* find the top-level task-local PVAR matching first_seg */
	obj = GetNextPviObject(TRUE);
	while( (obj = GetNextPviObject(FALSE)) != NULL ) {
		if( obj->type == POBJ_PVAR
		    && obj->ex.pv.task == task
		    && strcmp(obj->descriptor, first_seg) == 0 )
		{
			leaf = obj;
			break;
		}
	}
	if( leaf == NULL ) {
		char siblings[512];
		ListPvarSiblings( cpu, task, NULL, siblings, sizeof(siblings) );
		ReportError( REPORT_ERROR, "AddPvar",
			"Local PVAR '%s' not found under %s:%s. Available locals: %s",
			first_seg, cpu_descriptor, task_descriptor,
			siblings[0] ? siblings : "(none - task refused browse or no PVARs)" );
		return -1;
	}

	if( rest ) {
		leaf = TraverseStructMembers( cpu, leaf, rest );
		if( leaf == NULL ) return -1;
	}

	if( ValidatePvarLeaf( leaf, var_name ) != 0 ) return -1;

	leaf->watchsort = 0;
	WatchPviObject( leaf, TRUE );
	return 0;
}







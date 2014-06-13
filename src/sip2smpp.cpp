/**
*   \file main.cpp
*
*   \brief The main file of the SIP2SMPP project
*
*/

/**
*  \todo   1) use a log system (like : Log4c)
*          2) add a deamon mode
*/

#include <iostream>
#include <queue>

#include <sys/cdefs.h>
#include <sys/types.h>
#include <sys/socket.h>

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <getopt.h>
#include <sstream>

#include <pthread.h>
#include <semaphore.h>

#include "connectionSMPP.h"
#include "connectionSIP.h"

//#include "type_projet.h"
#include "ini/iniFile.h"
#include "log/log.h"
#include "net/smpp/struct_smpp.h"
#include "database.h"
#include "daemonize/daemonize.h"

using namespace std;

int running = 1;
char* pid_file = (char*)DEFAULT_PIDFILE;

/**
*  \brief This function is a signal handler function
*
*  \param value This parameter is a signal number
*
*/
void handler(int value){
    INFO(LOG_SCREEN | LOG_FILE,"The process has been terminated");
    log_destroy();
    exit(value);
}

/**
*  \brief This function print the help of this program
*
*  \param e  This parameter is used for define the signal number
*
*/
void usage(int8_t value){
    printf("%sHelp :%s\n", RED, END_COLOR);

    printf("sip2smpp [%soptions%s]  \n", CYAN, END_COLOR);
    printf("%s    -h  %s: help\n", CYAN, END_COLOR);
    printf("%s    -v  %s: show version\n", CYAN, END_COLOR);
    printf("%s    -D  %s: debug level (0-8)\n", CYAN, END_COLOR);
    printf("%s    -f  %s: use fork (parameter 1) | Not implemented\n", CYAN, END_COLOR);
    printf("%s    -P  %s: PID file. Default PID file is [%s]\n", CYAN, END_COLOR, DEFAULT_PIDFILE);
    printf("%s    -c  %s: config file to use to specify some options. Default location is [%s]\n", CYAN, END_COLOR, DEFAULT_CONFIG);
    printf("%s    -l  %s: log file to use to specify some options. Default location is [%s]\n", CYAN, END_COLOR, DEFAULT_CONFIG);

    handler(value);
}

/**
*  CONVERSIONS :
*    - trame SIP -> struct SMS
*/

#include "parseSip.h"
#include "createMessageSip.h"

std::vector<std::string> &split(const std::string &s, char delim, std::vector<std::string> &elems) {
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, delim)) {
        elems.push_back(item);
    }
    return elems;
}

std::vector<std::string> split(const std::string &s, char delim) {
    std::vector<std::string> elems;
    split(s, delim, elems);
    return elems;
}

SMS* sip2sms(string str){
        std::vector<std::string> explode = split(str,'\n');
        int i = 0; // search FROM in the SIP trame
        while(strncmp((char*)explode[i++].c_str(),(char*)"From",4)!=0);
        Sip_From from(explode[i-1]);

        i = 0;// search TO in the SIP trame
        while(strncmp((char*)explode[i++].c_str(),(char*)"To",2)!=0);
        Sip_To to(explode[i-1]);

        SMS *sms = new SMS();
        sms->src = (char*)malloc(sizeof(char)*21);
        sms->dst = (char*)malloc(sizeof(char)*21);
        sms->msg = (char*)malloc(sizeof(char)*256);
	memset(sms->src, 0, sizeof(char)*21);
	memset(sms->dst, 0, sizeof(char)*21);
	memset(sms->msg, 0, sizeof(char)*256);
	strcpy(sms->src,(char*)from.get_user_name().c_str());
	strcpy(sms->dst,(char*)to.get_user_name().c_str());
	strcpy(sms->msg,(char*)explode[explode.size()-1].c_str());

        return sms;
}

/**
*  Init Connection
*/
Connection_SMPP *smpp = NULL;
Connection_SIP  *sip = NULL;

/**
 * size_db : allow to decrease SQL queries
 */
static unsigned int size_smpp = 0;
static unsigned int size_sip  = 0;

/**
* CONNEXION SMPP
*/
sem_t mutex_smpp;

/**
*  \brief This function is used for send all SMS of the DB (SMPP)
*/
static void* gestionSMPP_send(void *data){
	sem_wait(&mutex_smpp);
	
	if(size_smpp>0){
		SMS *sms = (SMS*)malloc(sizeof(SMS));
		memset(sms,0,sizeof(SMS));
		
		sms_get(DB_TYPE_SMPP,sms);
		if(sms){
			smpp->sendSMS(*sms);
			sms_rm(sms);
			size_smpp--;
		}else{
			sms_cls(sms);
		}
		free_sms(&sms);
	}
	
	sem_post(&mutex_smpp);
	return 0;
}

/**
*  \brief This function is used for transfer all SMS received to the DB (SMPP->SIP)
*/
static void* gestionSMPP_listend(void *data){
	SMS *sms = NULL;
	sem_wait(&mutex_smpp);
	
	sms = smpp->receiverSMS(true);
	if(sms && (sms->dst) && (sms->src) && (sms->msg)){
		Connection_SMPP::displaySMS(*sms);
		sms_set(DB_TYPE_SIP,sms->src,sms->dst,sms->msg);
		size_sip++;
	}	
	free_sms(&sms);

	sem_post(&mutex_smpp);
	return 0;
}

/**
*  \brief This function is used for managed all input and output SMPP trafic
*/
int temp_smpp = 0;
static void* gestionSMPP(void *data){
    while(running){
	smpp = new Connection_SMPP(smppConnectIni.smpp_server_ip,smppConnectIni.smpp_server_port,
			smppConnectIni.user_smpp,smppConnectIni.pass_smpp,BIND_TRANSCEIVER,true);
 	sem_init(&mutex_smpp, 0, 1);
	
	while(smpp && smpp->connect && running){
		temp_smpp = 0;
		pthread_t thread_listend;
		pthread_t thread_send;

		pthread_create(&thread_listend,NULL,gestionSMPP_listend,NULL);
		pthread_create(&thread_send,NULL,gestionSMPP_send,NULL);
		
		pthread_join(thread_listend,NULL);
		pthread_join(thread_send,NULL);
	}

	if(smpp){
		delete smpp;
		smpp = NULL;
	}
	sem_destroy(&mutex_smpp);
	sleep(2*temp_smpp++);
    }
    return 0;
}

/**
*  CONNEXION SIP
*/
sem_t mutex_sip;

/**
*  \brief This function is used for transfer all SMS received to the SMS listend FIFO (SIP)
*/
static void* gestionSIP_listend(void *data){
	sem_wait(&mutex_sip);
	
	char* str = sip->receiveSIP();
	if(str){
		SMS *sms = sip2sms(str);
		if(sms && (sms->dst) && (sms->src) && (sms->msg)){
			sms_set(DB_TYPE_SMPP,sms->src,sms->dst,sms->msg);
			size_smpp++;
		}

		string sipOk = createSip200(sipDestIni.sip_dest_ip, sipDestIni.sip_dest_port,
					    sipLocalIni.sip_local_ip, sipLocalIni.sip_local_port,
					    sms->dst, sms->src, getCallID(str));
		sip->sendSIP(sipOk, sipDestIni.sip_dest_ip, sipDestIni.sip_dest_port);

		free_sms(&sms);
		free(str);        
	}
	
	sem_post(&mutex_sip);
	return 0;
}

/**
*  \brief This function is used for send all SMS of the SMS send FIFO (SIP)
*/
static void* gestionSIP_send(void *data){
	sem_wait(&mutex_sip);
	
	if(size_sip>0){
		SMS *sms = (SMS*)malloc(sizeof(SMS));
		memset(sms,0,sizeof(SMS));
		sms_get(DB_TYPE_SIP,sms);
		
		if(!sms || !(sms->dst) || !(sms->src) || !(sms->msg)){
    			ERROR(LOG_SCREEN | LOG_FILE,"SMS failed...");
			free_sms(&sms);
			return 0;
		}
		
		string str = createTrameSipSMS(sipDestIni.sip_dest_ip, sipDestIni.sip_dest_port,
					sipLocalIni.sip_local_ip, sipLocalIni.sip_local_port,
					sms->dst, sms->src, sms->msg);
		
		if(str.size()>0){
			//cout << "mainIni.sip_dest_ip   = " << mainIni.sip_dest_ip << endl;
			//cout << "mainIni.sip_dest_port = " << mainIni.sip_dest_port << endl;
			sip->sendSIP(str, sipDestIni.sip_dest_ip, sipDestIni.sip_dest_port);
			//sms_cls(sms);
			sms_rm(sms);
			size_sip--;
		}else{
			sms_cls(sms);
		}
		free_sms(&sms);
	}
	
	sem_post(&mutex_sip);
	return 0;
}

/**
*  \brief This function is used for managed all input and output SIP trafic
*/
int temp_sip = 0;
static void* gestionSIP(void *data){
    while(running){
	sip = new Connection_SIP(sipLocalIni.sip_local_ip,sipLocalIni.sip_local_port,SIP_TRANSCEIVER,true);
 	sem_init(&mutex_sip, 0, 1);
	/* initialize mutex to 1 - binary semaphore   */
	/* second param = 0      - semaphore is local */

	while( sip && sip->connect && running ){
		temp_sip = 0;
		pthread_t thread_listend;
                pthread_t thread_send;

                pthread_create(&thread_listend,NULL,gestionSIP_listend,NULL);
                pthread_create(&thread_send,NULL,gestionSIP_send,NULL);

                pthread_join(thread_listend,NULL);
                pthread_join(thread_send,NULL);
	}

	if(sip){
		delete sip;
		sip = NULL;
	}

	sem_destroy(&mutex_sip);
	sleep(2*temp_sip++);
    }
    return 0;
}

static void* checkDB(void *data){
/*    while(running){
        if(dbi_conn_ping(conn)==0){
            ERROR(LOG_FILE | LOG_SCREEN, "Reconnect to the DB...");
            printf("%sDBMS             %s: [%s]\n", GREEN, END_COLOR, dbmsIni.dbms_name);
            printf("%sDB dir name      %s: [%s]\n", GREEN, END_COLOR, dbmsIni.db_dirname);
            printf("%sDB base name     %s: [%s]\n", GREEN, END_COLOR, dbmsIni.db_basename);
        }
        sleep(2);
    }*/
    return 0;
}

/**
*  \brief Main funtion
*
*  \param argc This parameter is used to hold the number of arguments that we passed on the command line 
*  \param argv This parameter is an array of char pointer containing the value of arguments
*
*/
int main(int argc,char **argv){
    int c, nofork=1;
    char *conffile = NULL;
    log_init("logFile",NULL);
    log2display(LOG_ALERT);

    while((c=getopt(argc, argv, "c:vp:fhD:"))!=-1) {
        switch(c) {
            case 'c':
                    conffile = optarg;
                    break;
            case 'v':
                    printf("sip2smpp version: %s\n", VERSION);
                    exit(0);
                    break;
            case 'P':
                    pid_file = optarg;
                    break;
            case 'f':
                    nofork = 0;
                    log2display(LOG_NONE);
                    break;
            case 'h':
                    usage(0);
                    break;
	    case 'D':
                    {
                      char log = atoi(optarg);
                      if(log >= 0 && log <= 8){
                         printf("%d\n",log);
                         log2display((Loglevel)log);
                      }
                      break;
                    }
            default:
                    abort();
        }
    }

    if(!conffile){
        conffile = (char*)malloc(sizeof(char)*strlen(DEFAULT_CONFIG)+1);
        strcpy(conffile,DEFAULT_CONFIG);
    }

    if(FILE *file = fopen(conffile,"r")){
        fclose(file);
    }else{
        ERROR(LOG_FILE | LOG_SCREEN,"The INI file isn't found!");
        handler(-1);
    }

    if(!loadFileIni(conffile,SECTION_ALL)){
        ERROR(LOG_FILE | LOG_SCREEN,"There are errors in the INI file!");
        handler(-1);
    }

    if(daemonize(nofork) != 0){
        ERROR(LOG_FILE | LOG_SCREEN,"Daemoniize failed");
        exit(-1);
    }

    printf("%sDB dir name      %s: [%s]\n", GREEN, END_COLOR, dbmsIni.db_dirname);
    if(db_init() == -1){
	ERROR(LOG_FILE | LOG_SCREEN,"There are errors when the DB connection!");
	handler(-1);
    }else{
	size_sip  = sms_count(DB_TYPE_SIP);
	size_smpp = sms_count(DB_TYPE_SMPP);
    }

    printf("%sVersion          %s: [%s]\n", GREEN, END_COLOR, VERSION);
    printf("%sPid file         %s: [%s]\n", GREEN, END_COLOR, pid_file);
    printf("-------     %s     -------\n" , conffile);
    printf("%sSIP dest IP      %s: [%s]\n", GREEN, END_COLOR, sipDestIni.sip_dest_ip);
    printf("%sSIP dest Port    %s: [%s]\n", GREEN, END_COLOR, sipDestIni.sip_dest_port);
    printf("%sSIP Local IP     %s: [%s]\n", GREEN, END_COLOR, sipLocalIni.sip_local_ip);
    printf("%sSIP Local Port   %s: [%s]\n", GREEN, END_COLOR, sipLocalIni.sip_local_port);
    printf("\n");
    printf("%sSMPP Server IP   %s: [%s]\n", GREEN, END_COLOR, smppConnectIni.smpp_server_ip);
    printf("%sSMPP Server Port %s: [%s]\n", GREEN, END_COLOR, smppConnectIni.smpp_server_port);
    printf("\n");
    printf("%sDBMS             %s: [%s]\n", GREEN, END_COLOR, dbmsIni.dbms_name);
    printf("%sDB dir name      %s: [%s]\n", GREEN, END_COLOR, dbmsIni.db_dirname);
    printf("%sDB base name     %s: [%s]\n", GREEN, END_COLOR, dbmsIni.db_basename);

    pthread_t transceiverSMPP;
    pthread_t transceiverSIP;
    pthread_t checkDBconnection;

    pthread_create(&transceiverSMPP,NULL,gestionSMPP,NULL);
    pthread_create(&transceiverSIP,NULL,gestionSIP,NULL);
    pthread_create(&checkDBconnection,NULL,checkDB,NULL);

//  command line system (not finished)
    string str = "";
    while(str != "shutdown"){
	cin >> str;
	if(str == "help"){
		cout << "List of commands :"                            << endl;
		cout << "  help        : display the commands"          << endl;
		cout << "  sms         : create a SMS to send"          << endl;
		cout << "  size_list   : display the sizes of list SMS" << endl;
		cout << "  reload_sip  : reload SIP"                    << endl;
		cout << "  reload_smpp : reload SMPP"                   << endl;
		cout << "  log         : choice the log level"          << endl;
		cout << "  shutdown    : exit the program"              << endl;
	}
	if(str == "sms"){
		short i = 1;
		char src[25], dst[25], msg[160], type[5];
		cout << "From           : ";  cin >> src;
		cout << "To             : ";  cin >> dst;
		cout << "Msg            : ";  cin >> msg;
		cout << "Type(sip/smpp) : ";  cin >> type;
		cout << "How many       : ";  cin >> i;
		if(strcmp(type,"sip")==0){
		    while(i-- > 0){
			sms_set(DB_TYPE_SIP,(const char*)&src,(const char*)&dst,(const char*)&msg);
			size_sip++;
		    }
		}else if(strcmp(type,"smpp")==0){
		    while(i-- > 0){
			sms_set(DB_TYPE_SMPP,(const char*)&src,(const char*)&dst,(const char*)&msg);
                        size_smpp++;
		    }
		}else{
			printf("Type is wrong\n");
		}
	}
	if(str == "size_list"){
		cout << "size_smpp    : " << size_smpp    << endl;
		cout << "size_sip     : " << size_sip     << endl;
	}
	if(str == "reload_sip"){
	   if(!loadFileIni(conffile,SECTION_SIP)){
	        ERROR(LOG_FILE | LOG_SCREEN,"There are errors in the INI file!\n");
	   }else{
		if(sip){
			delete sip;
			sip = NULL;
		}
		temp_sip = 0;
	   	sleep(1);
           	pthread_create(&transceiverSIP,NULL,gestionSIP,NULL);
           }
	}
	if(str == "reload_smpp"){
	   if(!loadFileIni(conffile,SECTION_SMPP)){
	        ERROR(LOG_FILE | LOG_SCREEN,"There are errors in the INI file!\n");
	   }else{
		if(sip){
			delete sip;
			sip = NULL;
		}
		temp_smpp = 0;
	   	sleep(1);
           	pthread_create(&transceiverSIP,NULL,gestionSIP,NULL);
           }
	}
	if(str == "log"){
		int lvl = 0;
		printf("log lvl : ");
		scanf("%d", &lvl);
		log2display((Loglevel)lvl);
	}
    }//End While
	
    running = 0;

/*
    if(smpp) {
       delete smpp;
       smpp = NULL;
    }
    if(sip)  {
       delete sip;
       sip = NULL;
    }
*/

    pthread_join(transceiverSMPP,NULL);
    pthread_join(transceiverSIP,NULL);
    pthread_join(checkDBconnection,NULL);

    freeFileIni(SECTION_ALL);

    handler(0);
    return 0;
}
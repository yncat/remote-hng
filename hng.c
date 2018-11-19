//Hangman network challenge
//Syspro final
//71646673 - Yukio Nozawa

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <pthread.h>
#include <sys/stat.h>

//constants
#define MAX_WORDLEN 20
#define MODE_HOST 0
#define MODE_JOIN 1
#define USING_PORT 15000

//structs
struct hng_game{
	char answer[MAX_WORDLEN+2];
	int answer_len;
	int tries;
	char answered_switch[26];
	char ip[64];
	int fd;
	FILE* logFile;
};

//Prototype functions
//Startup related
int hng_checkArgs(struct hng_game* game,int,char*[]);
int hng_selectMode();

//Client side functions
void hng_clientSide(struct hng_game*);
void hng_clientSide_setIp(struct hng_game*);

//Server side functions
void hng_serverSide(struct hng_game*);
void hng_serverSide_game(struct hng_game*,int);
void hng_serverSide_sendToClient(struct hng_game*,char*);
int hng_serverSide_receiveFromClient(struct hng_game*,char*);
void hng_serverSide_closeConnection(struct hng_game*);

//Hangman related
void hng_initStruct(struct hng_game*);
int hng_setAnswer(struct hng_game*);
void hng_displayStatus(struct hng_game*);
int hng_input(struct hng_game*,char*);
int hng_checkInput(struct hng_game*,char*);
void hng_try(struct hng_game*,char*);
void hng_answer(struct hng_game*,char*);
void hng_answerSuccess(struct hng_game*);
void hng_answerFail(struct hng_game*,char*);
int hng_validateInput(char*);
int hng_validateTry(struct hng_game*,char*);
void hng_trim(char*);

//thread
void* hng_clientSide_input(void* prm){
	int* fd_client_pointer=(int*)prm;
	int fd_client=*fd_client_pointer;
	while(1){
		char buf[1024];
		memset(buf,0,1024);
		scanf("%1024s",buf);
		write(fd_client,buf,strlen(buf));
	}
}

//Entry point starts here
int main(int argc, char* argv[]){
	struct hng_game game;
	hng_initStruct(&game);
	int mode=hng_checkArgs(&game,argc,argv);
	if(mode==MODE_JOIN){
		hng_clientSide(&game);
	}else{
		hng_serverSide(&game);
	}
	return 0;
}

int hng_checkArgs(struct hng_game* game, int argc, char* argv[]){//Checks the commandline arguments and determines the game mode
	if(argc==1) return hng_selectMode();
	if(strcmp(argv[1],"host")==0){
		if(argc>2) strncpy(game->answer,argv[2],20);
		return MODE_HOST;
	}
	if(strcmp(argv[1],"join")==0){
		if(argc>2) strncpy(game->ip,argv[2],64);
		return MODE_JOIN;
	}
	return hng_selectMode();
}

int hng_selectMode(){//Have the user input the desired mode
	int ret;
	while(1){
		char input[20];
		printf("hng: type host or join> ");
		scanf("%20s",input);
		if(strcmp(input,"host")==0){
			ret=MODE_HOST;
			break;
		}
		if(strcmp(input,"join")==0){
			ret=MODE_JOIN;
			break;
		}
	}
	return ret;
}

void hng_clientSide(struct hng_game* game){//Client-side main routine
	printf("hng: You're joining an existing server.\n");
	char ip[64];
	if(strlen(game->ip)==0) hng_clientSide_setIp(game);//game->ip may be filled with something if the corresponding commandline parameter is specified
	//Name resolution ( from http://www.geekpage.jp/programming/linux-network/book/02/2-19.php)
	struct addrinfo hints, *res;
	struct in_addr addr;
	char new_ip[32];
	memset(&hints,0,sizeof(hints));
	hints.ai_socktype=SOCK_STREAM;
	hints.ai_family=AF_INET;
	if(getaddrinfo(game->ip,NULL,&hints,&res)!=0){//error
		strncpy(new_ip,game->ip,64);
	}else{
		addr.s_addr=((struct sockaddr_in*)(res->ai_addr))->sin_addr.s_addr;
		inet_ntop(AF_INET,&addr,new_ip,sizeof(new_ip));
	}
	if(strcmp(game->ip,new_ip)==0){//Did not resolve
		printf("hng: Connecting to %s:%d... ",game->ip,USING_PORT);
	}else{//Resolved
		printf("hng: Connecting to %s(%s):%d... ",game->ip,new_ip,USING_PORT);
		memset(game->ip,0,64);
		strcpy(game->ip,new_ip);
	}
	freeaddrinfo(res);
	//Name resolution ends here

	//Socket setup starts here
	int fd_socket;
	struct sockaddr_in client;
	fd_socket=socket(PF_INET,SOCK_STREAM,0);
	memset(&client,0,sizeof(client));
	client.sin_family=PF_INET;
	client.sin_addr.s_addr=inet_addr(game->ip);
	client.sin_port=htons(USING_PORT);
	if(connect(fd_socket,(struct sockaddr *)&client,sizeof(client))==-1){//Couldn't connect
		printf("Could not connect.\n");
		close(fd_socket);
		return;
	}
	printf("Connected!\n");
	//The following thread takes care of stdin
	pthread_attr_t attr;
	pthread_attr_init (&attr);
	pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);//always detatch threads
	pthread_t thread;
	pthread_create(&thread,&attr,hng_clientSide_input,(void*)&fd_socket);//create a thread, passing the file descriptor of the socket to it
	while(1){//Receive data from server and copy them to stdout
		char buf[1024];
		memset(buf,0,1024);
		read(fd_socket,buf,1024);
		write(STDOUT_FILENO,buf,strlen(buf));
		if(strstr(buf,"Closing connection")!=NULL) break;//The word "Closing connection" is o. ma. ji. na. i
	}
}

void hng_clientSide_setIp(struct hng_game* game){
	printf("     Where do you want to connect to? >");
	scanf("%64s",game->ip);
}

void hng_serverSide(struct hng_game* game){
	hng_setAnswer(game);//Server must have the answer set
	printf("hng: Initializing connection...\n");
	//Socket setup starts here
	int fd_socket;
	struct sockaddr_in server;
	fd_socket=socket(PF_INET, SOCK_STREAM, 0);
	memset(&server, 0, sizeof(server));
	server.sin_family=PF_INET;
	server.sin_port   = htons(USING_PORT);
	server.sin_addr.s_addr=htonl(INADDR_ANY);
	bind(fd_socket,(struct sockaddr *)&server, sizeof(struct sockaddr_in));
	printf("     Waiting connection at port %d...\n",USING_PORT);
	listen(fd_socket,5);
	while(1){
		waitpid(0,NULL,WNOHANG);//Kill a zombie if any (I do know that it's better to run a dedicated thread for sweeping zombies, but just don't care this time)
		struct sockaddr_in client;
		memset(&client, 0, sizeof(client));
		int len=sizeof(client);
		int fd_client=accept(fd_socket,(struct sockaddr*)&client, &len);
		pid_t pid=fork();
		if(pid==0){
			hng_serverSide_game(game,fd_client);
			return;
		}
	}//end while
}

void hng_serverSide_game(struct hng_game* game, int fd_client){
	game->fd=fd_client;
	//Determine the name of log file to write
	char logFileName[100];
	int logFileNum=1;
	while(1){
		sprintf(logFileName,"hng-%d.log",logFileNum);
		struct stat st;
		int ret=stat(logFileName,&st);
		if(ret!=0) break;//This file name can be used
		logFileNum++;
	}
	game->logFile=fopen(logFileName,"w");

	hng_serverSide_sendToClient(game,"hng: session: Hello, let's start a hangman game!\n");
	while(1){
		hng_displayStatus(game);
		char ans[1024];
		int disconnected=hng_input(game,ans);
		if(disconnected==1) break;
		if(hng_checkInput(game,ans)==1) continue;
		break;
	}//end game loop
	close(game->fd);
}

void hng_serverSide_sendToClient(struct hng_game* game, char* buf){
	write(game->fd,buf,strlen(buf));
	fputs(buf,game->logFile);
}

int hng_serverSide_receiveFromClient(struct hng_game* game,char* out){
	//returns 1 if the client is disconnected, otherwise 0
	memset(out,0,1024);
	int ret=recv(game->fd,out,1024,0);
	if(ret==0){
		hng_serverSide_closeConnection(game);
		return 1;
	}
	fputs(out,game->logFile);
	fputs("\n",game->logFile);
return 0;
}

void hng_serverSide_closeConnection(struct hng_game* game){
	close(game->fd);
	fclose(game->logFile);
}

void hng_initStruct(struct hng_game* game){//initializes the given struct for startup
	memset(game->ip,0,64);
	game->tries=0;
	memset(game->answer,0,MAX_WORDLEN+2);//cr+lf or lf is inserted so not to overflow
	memset(game->answered_switch,0,26);//a to z
	game->fd=STDIN_FILENO;
}

int hng_setAnswer(struct hng_game* game){//returns 1 when successfully set, otherwise 0
	if(strlen(game->answer)>0){
		game->answer_len=strlen(game->answer);
		printf("hng: Using the answer \"%s\" from the commandline parameter, which is %d chars long.\n",game->answer,game->answer_len);
		return 1;
	}
	while(1){
		hng_initStruct(game);
		printf("hng: Set the answer >");
		fflush(STDIN_FILENO);//Linux seems to queue stdout until lf comes, but I don't want that to happen here
		read(STDIN_FILENO,game->answer,MAX_WORDLEN+2);//lf or crlf welcomed
		hng_trim(game->answer);
		game->answer_len=strlen(game->answer);
		if(game->answer_len<3){//hangman with 2 characters? Not fun at all
			printf("hng: error: More than 3 characters please.\n");
			continue;
		}
			if(hng_validateInput(game->answer)==0){//Contains invalid chars
			printf("hng: error: Lowercase alphabets only please!\n");
			continue;
		}
	printf("hng: The answer is %s, which is %d chars long.\n",game->answer,game->answer_len);
		printf("     Confirm this answer and procede? y/n> ");
		fflush(STDIN_FILENO);
		char yn;
		read(STDIN_FILENO,&yn,1);
		while(getchar()==EOF){}//Always return-key-codes, sigh
		if(yn==121) break;//if "y" is pressed
	}//end while
	return 1;
}

void hng_displayStatus(struct hng_game* game){//displays the current status before asking for answer input
	char tmpstr[1024];
	sprintf(tmpstr,"hng: session: status: try %d, you have picked ",game->tries+1);
	hng_serverSide_sendToClient(game,tmpstr);
	int i,count=0;
	for(i=0;i<26;i++){//Search for alphabets that you already picked
		if(game->answered_switch[i]){// have picked this alphabet
			count++;
			char tmpstr[1024];
			sprintf(tmpstr,"%c ",97+i);
			hng_serverSide_sendToClient(game,tmpstr);
		}//have picked?
	}//end for
	if(count==0){//nothing has picked
		hng_serverSide_sendToClient(game,"none");
	}else{
		char tmpstr[1024];
		sprintf(tmpstr,"(total %d)",count);
		hng_serverSide_sendToClient(game,tmpstr);
	}
	hng_serverSide_sendToClient(game,"\n");
	//display the currently discovered answer
	hng_serverSide_sendToClient(game,"     ");
	for(i=0;i<game->answer_len;i++){
		if(game->answered_switch[game->answer[i]-97]){//already discovered
			char tmpstr[1024];
			sprintf(tmpstr,"%c",game->answer[i]);
			hng_serverSide_sendToClient(game,tmpstr);
		}else{//not yet
			hng_serverSide_sendToClient(game,"*");
		}
	}
	hng_serverSide_sendToClient(game,"\n");
}

int hng_input(struct hng_game* game, char* ans_buf){//Have the player input his answer
	//returns 1 if the client is disconnected, otherwise 0
	int ret;
	while(1){
		memset(ans_buf,0,1024);
		hng_serverSide_sendToClient(game,"hng: session: Try or answer> ");
		ret=hng_serverSide_receiveFromClient(game,ans_buf);
		if(ret==1) break;//client disconnected
		hng_trim(ans_buf);
		int ans_buf_len=strlen(ans_buf);
		if(ans_buf_len<=0){
			hng_serverSide_sendToClient(game,"hng: session: error: Input something!\n");
			continue;
		}
		if(ans_buf_len==2){
			hng_serverSide_sendToClient(game,"hng: session: error: Input of 2 chars is obviously pointless in this application.\n");
			continue;
		}
		if(hng_validateInput(ans_buf)==0){
			hng_serverSide_sendToClient(game,"hng: session: error: Lowercase alphabets only please!\n");
			continue;
		}
		if(ans_buf_len>1 && ans_buf_len!=game->answer_len){
			hng_serverSide_sendToClient(game,"hng: session: error: The number of characters does not match, try again.\n");
			continue;
		}
		if(ans_buf_len==1 && hng_validateTry(game,ans_buf)==0){
			char tmpstr[1024];
			sprintf(tmpstr,"hng: session: error: You have already picked %s.\n",ans_buf);
			hng_serverSide_sendToClient(game,tmpstr);
			continue;
		}
		break;
	}//end while
	return ret;
}

int hng_checkInput(struct hng_game* game, char* ans){//check and process the input from the player
	//returns 1 if the game still continues, otherwise 0
	if(strlen(ans)==1){
		hng_try(game,ans);
		return 1;
	}else{
		hng_answer(game,ans);
	}
	return 0;
}

void hng_try(struct hng_game* game, char* ans){//try to discover a hint
	//find if the specified character is included in the answer
	int i,found=0;
	for(i=0;i<game->answer_len;i++){
		if(ans[0]==game->answer[i]){//found!
			found++;
		}//found?
	}//end for
	if(found==1){
		char tmpstr[1024];
		sprintf(tmpstr,"hng: session: try: There is %d %s!\n",found,ans);
		hng_serverSide_sendToClient(game,tmpstr);
	}else if(found>1){
		char tmpstr[1024];
		sprintf(tmpstr,"hng: session: try: There are %d %s's!\n",found,ans);
		hng_serverSide_sendToClient(game,tmpstr);
	}else{
		char tmpstr[1024];
		sprintf(tmpstr,"hng: session: try: There is no %s!\n",ans);
		hng_serverSide_sendToClient(game,tmpstr);
	}
	game->tries++;
	game->answered_switch[ans[0]-97]=1;
}

void hng_answer(struct hng_game* game, char* ans){//Compares the input answer and the true one
	if(strcmp(game->answer,ans)==0){
		hng_answerSuccess(game);
	}else{
		hng_answerFail(game,ans);//Need to path the input answer since I want to measure the percentage of correctness of the answer
	}
}

void hng_answerSuccess(struct hng_game* game){//The answer is true
	hng_serverSide_sendToClient(game,"HNG: session: Yes! That's right! Congrats!\n              Closing connection...\n");
	hng_serverSide_closeConnection(game);
}

void hng_answerFail(struct hng_game* game, char* ans){//The answer is not true
	char tmpstr[1024];
	float total=(float)game->answer_len;
	float correct=0;
	int i;
	for(i=0;i<game->answer_len;i++){
		if(game->answer[i]==ans[i]) correct++;
	}
	float percent=correct/total*100;
	sprintf(tmpstr,"HNG: session: No! That's %.0f%% correct, but not right! Dahahahahahahaha, bye bye!\n              Closing connection...\n",percent);
	hng_serverSide_sendToClient(game,tmpstr);
	hng_serverSide_closeConnection(game);
}

int hng_validateInput(char* input){//Checks if the specified input is valid for hangman, in other words, if it really consists of lowercase alphabets
	int i, avl=1;
	for(i=0;i<strlen(input);i++){//Accept lowercase alphabets only
		if(input[i]<97 || input[i]>122){//Unavailable char detected
			avl=0;
			break;
		}//Unavailable?
	}//end for
	return avl;
}

void hng_trim(char* trm){//Trims any cr or lf's from the specified input
	int c=strlen(trm)-1;
	while(1){
		if(c==0) break;
		if(trm[c]!=13 && trm[c]!=10) break;
		memset(trm+c,0,1);
		c--;
	}
}

int hng_validateTry(struct hng_game* game, char* input){//Checks if the specified character can be picked in the current context
	if(game->answered_switch[input[0]-97]==1){
		return 0;//already picked, so no
	}
	return 1;
}

//EOF
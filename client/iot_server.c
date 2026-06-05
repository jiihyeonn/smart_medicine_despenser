/* 서울기술교육센터 AIoT */
/* author : KSH */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/time.h>
#include <time.h>
#include <errno.h>

#define BUF_SIZE 512          /* 메시지 버퍼 확대 (기존 100 → 512) */
#define MAX_CLNT 34
#define ID_SIZE 10
#define ARR_CNT 5

#define DEBUG
typedef struct {
		char fd;
		char *from;
		char *to;
		char *msg;
		int len;
}MSG_INFO;

typedef struct {
		int index;
		int fd;
		char ip[20];
		char id[ID_SIZE];
		char pw[ID_SIZE];
}CLIENT_INFO;

void * clnt_connection(void * arg);
void send_msg(MSG_INFO * msg_info, CLIENT_INFO * first_client_info);
void error_handling(char * msg);
void log_file(char * msgstr);
void getlocaltime(char * buf);

int clnt_cnt=0;
pthread_mutex_t mutx;

int main(int argc, char *argv[])
{
		int serv_sock, clnt_sock;
		struct sockaddr_in serv_adr, clnt_adr;
		int clnt_adr_sz;
		int sock_option  = 1;
		pthread_t t_id[MAX_CLNT] = {0};
		int str_len = 0;
		int i=0;
		char idpasswd[(ID_SIZE*2)+3];
		char *pToken;
		char *pArray[ARR_CNT]={0};
		char msg[BUF_SIZE];

		FILE * idFd = fopen("idpasswd.txt","r");
		if(idFd == NULL)
		{
			perror("fopen(\"idpasswd.txt\",\"r\") ");
			exit(1);
		}
		char id[ID_SIZE];
		char pw[ID_SIZE];
		CLIENT_INFO * client_info = (CLIENT_INFO *)calloc(sizeof(CLIENT_INFO),MAX_CLNT);
		if(client_info == NULL)
		{
			perror("calloc()");
			exit(1);
		}
		do {
			str_len = fscanf(idFd,"%s %s",id,pw);	
			if(str_len <= 0)
				break;
			client_info[i].fd=-1;
			strcpy(client_info[i].id,id);
			strcpy(client_info[i].pw,pw);
			i++;
			if(i > MAX_CLNT)
			{
				printf("error client_info pull(Max:%d)\n",MAX_CLNT);			
				break;
			}
		} while(1);
		fclose(idFd);

		if(argc != 2) {
				printf("Usage : %s <port>\n",argv[0]);
				exit(1);
		}
		fputs("IoT Server Start!!\n",stdout);

		if(pthread_mutex_init(&mutx, NULL))
				error_handling("mutex init error");

		serv_sock = socket(PF_INET, SOCK_STREAM, 0);

		memset(&serv_adr, 0, sizeof(serv_adr));
		serv_adr.sin_family=AF_INET;
		serv_adr.sin_addr.s_addr=htonl(INADDR_ANY);
		serv_adr.sin_port=htons(atoi(argv[1]));

		setsockopt(serv_sock, SOL_SOCKET, SO_REUSEADDR, (void*)&sock_option, sizeof(sock_option));
		if(bind(serv_sock, (struct sockaddr *)&serv_adr, sizeof(serv_adr))==-1)
				error_handling("bind() error");

		if(listen(serv_sock, 5) == -1)
				error_handling("listen() error");

		while(1) {
				clnt_adr_sz = sizeof(clnt_adr);
				clnt_sock = accept(serv_sock, (struct sockaddr *)&clnt_adr, &clnt_adr_sz);
				if(clnt_cnt >= MAX_CLNT)
				{
						printf("socket full\n");
						shutdown(clnt_sock,SHUT_WR);
						continue;
				}
				else if(clnt_sock < 0)
				{
						perror("accept()");
						continue;
				}

				str_len = read(clnt_sock, idpasswd, sizeof(idpasswd));
				idpasswd[str_len] = '\0';

				if(str_len > 0)
				{
						i=0;
						pToken = strtok(idpasswd,"[:]");

						while(pToken != NULL)
						{
								pArray[i] =  pToken;
								if(i++ >= ARR_CNT)
										break;	
								pToken = strtok(NULL,"[:]");
						}
						for(i=0;i<MAX_CLNT;i++)
						{
								if(!strcmp(client_info[i].id,pArray[0]))
								{
										if(client_info[i].fd != -1)
										{
												sprintf(msg,"[%s] Already logged!\n",pArray[0]);
												write(clnt_sock, msg,strlen(msg));
												log_file(msg);
												shutdown(clnt_sock,SHUT_WR);
#if 1   //for MCU
												client_info[i].fd = -1;
#endif  
												break;
										}
										if(!strcmp(client_info[i].pw,pArray[1])) 
										{

												strcpy(client_info[i].ip,inet_ntoa(clnt_adr.sin_addr));
												pthread_mutex_lock(&mutx);
												client_info[i].index = i; 
												client_info[i].fd = clnt_sock; 
												clnt_cnt++;
												pthread_mutex_unlock(&mutx);
												sprintf(msg,"[%s] New connected! (ip:%s,fd:%d,sockcnt:%d)\n",pArray[0],inet_ntoa(clnt_adr.sin_addr),clnt_sock,clnt_cnt);
												log_file(msg);
												write(clnt_sock, msg,strlen(msg));

												pthread_create(t_id+i, NULL, clnt_connection, (void *)(client_info + i));
												pthread_detach(t_id[i]);
												break;
										}
								}
						}
						if(i == MAX_CLNT)
						{
								sprintf(msg,"[%s] Authentication Error!\n",pArray[0]);
								write(clnt_sock, msg,strlen(msg));
								log_file(msg);
								shutdown(clnt_sock,SHUT_WR);
						}
				}
				else 
						shutdown(clnt_sock,SHUT_WR);

		}
		return 0;
}

void * clnt_connection(void *arg)
{
		CLIENT_INFO * client_info = (CLIENT_INFO *)arg;
		int str_len = 0;
		int index = client_info->index;
		char msg[BUF_SIZE];
		char to_msg[BUF_SIZE + 20 + 2];  /* 중계 버퍼: 원본 메시지 + 발신자ID 여유 */
		int i=0;
		char *pToken;
		char *pArray[ARR_CNT]={0};
		char strBuff[BUF_SIZE*2]={0};

		MSG_INFO msg_info;
		CLIENT_INFO  * first_client_info;

		first_client_info = (CLIENT_INFO *)((void *)client_info - (void *)( sizeof(CLIENT_INFO) * index ));
		
		while(1)
		{
				memset(msg, 0x0, sizeof(msg));
				str_len = read(client_info->fd, msg, sizeof(msg)-1); 
				if(str_len <= 0)
						break;

				msg[str_len] = '\0';

				// -------------------------------------------------------------------------
				// [방어벽 1] AND(앱) 세션 : 보낼 수 있는 경로 제한
				//   허용: AND → LOG (로그인/회원가입)
				//         AND → DOC (증상 전달)
				//   차단: AND → SQL 직접 접근 (DB 직접 조작 불가)
				// -------------------------------------------------------------------------
				if (strcmp(client_info->id, "AND") == 0)
				{
						char msg_copy[BUF_SIZE];
						strncpy(msg_copy, msg, BUF_SIZE - 1);
						msg_copy[BUF_SIZE - 1] = '\0';

						/* 수신자(목적지) ID 확인 - msg_copy로 파싱 (원본 msg 보존) */
						pToken = strtok(msg_copy, "[:]");
						if (pToken == NULL) { continue; }

						/* AND → SQL 직접 접근 차단 */
						if (strcmp(pToken, "SQL") == 0)
						{
								printf("[보안 경고] AND가 SQL로 직접 접근 시도 차단!\n");
								sprintf(to_msg, "[SERVER]ACCESS_DENIED : SQL 직접 접근 불가\n");
								write(client_info->fd, to_msg, strlen(to_msg));
								continue;
						}

						/* AND → LOG : 로그인/회원가입만 허용 */
						if (strcmp(pToken, "LOG") == 0)
						{
								pArray[0] = pToken;
								pArray[1] = strtok(NULL, "[:]");

								if (pArray[1] != NULL && (strncmp(pArray[1], "LOGIN", 5) == 0 ||
									strncmp(pArray[1], "CREATE", 6) == 0))
								{
										msg_info.fd = client_info->fd;
										msg_info.from = client_info->id;
										msg_info.to = pArray[0];
										/* msg 원본에서 [목적지] 를 [발신자] 로 교체해서 전달 */
										snprintf(to_msg, sizeof(to_msg), "[%s]%s",
											msg_info.from, msg + strlen(pArray[0]) + 2);
										msg_info.msg = to_msg;
										msg_info.len = strlen(to_msg);
										sprintf(strBuff, "msg : [%s->%s] %s\n", msg_info.from, msg_info.to, pArray[1]);
										log_file(strBuff);
										send_msg(&msg_info, first_client_info);
								}
								else
								{
										printf("[보안 경고] AND가 LOG로 허용되지 않은 명령 시도 차단!\n");
										sprintf(to_msg, "[SERVER]ACCESS_DENIED\n");
										write(client_info->fd, to_msg, strlen(to_msg));
								}
						}
						/* AND → DOC : 증상 전달만 허용 */
						else if (strcmp(pToken, "DOC") == 0)
						{
								pArray[0] = pToken;
								pArray[1] = strtok(NULL, "[:]");

								if (pArray[1] != NULL && strncmp(pArray[1], "SYMPTOM", 7) == 0)
								{
										msg_info.fd = client_info->fd;
										msg_info.from = client_info->id;
										msg_info.to = pArray[0];
										/* msg 원본에서 [목적지] 를 [발신자] 로 교체해서 전달 */
										snprintf(to_msg, sizeof(to_msg), "[%s]%s",
											msg_info.from, msg + strlen(pArray[0]) + 2);
										msg_info.msg = to_msg;
										msg_info.len = strlen(to_msg);
										sprintf(strBuff, "msg : [%s->%s] %s\n", msg_info.from, msg_info.to, pArray[1]);
										log_file(strBuff);
										send_msg(&msg_info, first_client_info);
								}
								else
								{
										printf("[보안 경고] AND가 DOC로 허용되지 않은 명령 시도 차단!\n");
										sprintf(to_msg, "[SERVER]ACCESS_DENIED\n");
										write(client_info->fd, to_msg, strlen(to_msg));
								}
						}
						else
						{
								sprintf(to_msg, "[SERVER]ACCESS_DENIED : 권한이 없습니다.\n");
								write(client_info->fd, to_msg, strlen(to_msg));
						}
						continue;
				}
				// -------------------------------------------------------------------------
				// [방어벽 2] 핵심 의약 데이터(MedicineCode, CumMedicine)는 DOC만 수정 가능
				//   UPDATEDB 또는 SETDB로 해당 컬럼 건드리면 DOC가 아닌 경우 차단
				// -------------------------------------------------------------------------
				else if ((strstr(msg, "UPDATEDB") != NULL || strstr(msg, "SETDB") != NULL) &&
				        (strstr(msg, "MedicineCode") != NULL || strstr(msg, "CumMedicine") != NULL))
				{
						if (strcmp(client_info->id, "DOC") != 0)
						{
								printf("[보안 경고] 비권한 사용자 [%s]의 핵심 의약 데이터 변경 시도 차단!\n", client_info->id);
								sprintf(to_msg, "[SERVER]ERROR@PERMISSION_DENIED\n");
								write(client_info->fd, to_msg, strlen(to_msg));
								continue;
						}
				}

				// -------------------------------------------------------------------------
				// 일반 중계 처리 (LOG / SQL / DOC 사이)
				// [목적지] 를 [발신자] 로 교체해서 원본 내용 통째로 전달
				// -------------------------------------------------------------------------
				pToken = strtok(msg, "[:]");   /* pToken = 목적지 ID */
				if (pToken == NULL) continue;

				msg_info.fd   = client_info->fd;
				msg_info.from = client_info->id;
				msg_info.to   = pToken;

				/* msg 원본에서 [목적지] 길이만큼 건너뛰고 발신자로 교체 */
				snprintf(to_msg, sizeof(to_msg), "[%s]%s",
					msg_info.from, msg + strlen(pToken) + 2);
				msg_info.msg = to_msg;
				msg_info.len = strlen(to_msg);

        /* 로그용: 짤리지 않은 전체 페이로드(내용) 추출 및 예쁘게 포맷팅 */
        char full_payload[BUF_SIZE];
        // 원본 msg에서 "[목적지]" 부분을 건너뛴 실제 내용만 복사
        strcpy(full_payload, msg + strlen(pToken) + 2); 

        // 송수신자를 보기 좋게 정렬하고 내용을 전부 출력
        sprintf(strBuff, " 💌 [ %-3s ➔ %-3s ] %s\n", 
        msg_info.from, msg_info.to, full_payload);
        log_file(strBuff);
				send_msg(&msg_info, first_client_info);
				
		}

		close(client_info->fd);

		sprintf(strBuff, "Disconnect ID:%s (ip:%s,fd:%d,sockcnt:%d)\n", client_info->id, client_info->ip, client_info->fd, clnt_cnt-1);
		log_file(strBuff);

		pthread_mutex_lock(&mutx);
		clnt_cnt--;
		client_info->fd = -1;
		pthread_mutex_unlock(&mutx);

		return 0;
}

void send_msg(MSG_INFO * msg_info, CLIENT_INFO * first_client_info)
{
		int i=0;

		if(!strcmp(msg_info->to,"ALLMSG"))
		{
				for(i=0;i<MAX_CLNT;i++)
						if((first_client_info+i)->fd != -1)	
								write((first_client_info+i)->fd, msg_info->msg, msg_info->len);
		}
		else if(!strcmp(msg_info->to,"IDLIST"))
		{
				char* idlist = (char *)malloc(ID_SIZE * MAX_CLNT);
				msg_info->msg[strlen(msg_info->msg) - 1] = '\0';
				strcpy(idlist,msg_info->msg);

				for(i=0;i<MAX_CLNT;i++)
				{
						if((first_client_info+i)->fd != -1)	
						{
								strcat(idlist,(first_client_info+i)->id);
								strcat(idlist," ");
						}
				}
				strcat(idlist,"\n");
				write(msg_info->fd, idlist, strlen(idlist));
				free(idlist);
		}
		else if(!strcmp(msg_info->to,"GETTIME"))
		{
			sleep(1);
			getlocaltime(msg_info->msg);
			write(msg_info->fd, msg_info->msg, strlen(msg_info->msg));
		}
		else
				for(i=0;i<MAX_CLNT;i++)
						if((first_client_info+i)->fd != -1)	
								if(!strcmp(msg_info->to,(first_client_info+i)->id))
										write((first_client_info+i)->fd, msg_info->msg, msg_info->len);
}

void error_handling(char *msg)
{
		fputs(msg, stderr);
		fputc('\n', stderr);
		exit(1);
}

void log_file(char * msgstr)
{
		fputs(msgstr,stdout);
}

void getlocaltime(char * buf)
{
	struct tm *t;
	time_t tt;
	char wday[7][4] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
	tt = time(NULL);
	if(errno == EFAULT)
		perror("time()");
	t = localtime(&tt);
	sprintf(buf,"[GETTIME]%02d.%02d.%02d %02d:%02d:%02d %s",t->tm_year+1900-2000,t->tm_mon+1,t->tm_mday,t->tm_hour,t->tm_min,t->tm_sec,wday[t->tm_wday]);
	return;
}
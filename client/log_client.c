/*
 * log_client.c - Login / Register relay client
 * Build: gcc -o log_client log_client.c -lpthread
 * Run:   ./log_client <IP> <port> LOG
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <pthread.h>

#define BUF_SIZE  256
#define NAME_SIZE 20
#define ARR_CNT   10

#define SQL_ID  "SQL"
#define AND_ID  "AND"

void* recv_msg(void* arg);
void  error_handling(char* msg);

char name[NAME_SIZE] = "[Default]";

typedef enum {
    FLOW_NONE = 0,
    FLOW_LOGIN,
    FLOW_REG_CHECK,
    FLOW_REG_INSERT
} FlowState;

FlowState g_flow       = FLOW_NONE;
char g_login_id[32]    = "";
char g_login_name[32]  = "";
char g_reg_id[32]      = "";
char g_reg_name[32]    = "";
char g_reg_age[8]      = "";
char g_reg_allergy[64] = "";

int main(int argc, char* argv[])
{
    int sock;
    struct sockaddr_in serv_addr;
    pthread_t rcv_thread;
    void* thread_return;
    char pass_msg[NAME_SIZE + 20];

    if (argc != 4) {
        printf("Usage : %s <IP> <port> <name>\n", argv[0]);
        exit(1);
    }
    sprintf(name, "%s", argv[3]);

    sock = socket(PF_INET, SOCK_STREAM, 0);
    if (sock == -1) error_handling("socket() error");

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family      = AF_INET;
    serv_addr.sin_addr.s_addr = inet_addr(argv[1]);
    serv_addr.sin_port        = htons(atoi(argv[2]));

    if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) == -1)
        error_handling("connect() error");

    sprintf(pass_msg, "[%s:PASSWD]", name);
    write(sock, pass_msg, strlen(pass_msg));

    printf("[%s] Server connected. Waiting for AND request...\n", name);

    pthread_create(&rcv_thread, NULL, recv_msg, (void*)&sock);
    pthread_join(rcv_thread, &thread_return);

    if (sock != -1) close(sock);
    return 0;
}

void* recv_msg(void* arg)
{
    int* sock  = (int*)arg;
    char name_msg[BUF_SIZE + NAME_SIZE + 1];
    char orig[BUF_SIZE + NAME_SIZE + 1];
    char send_buf[BUF_SIZE + NAME_SIZE + 2];
    char* pToken;
    char* pArray[ARR_CNT];
    int   i, str_len;

    while (1) {
        memset(name_msg, 0, sizeof(name_msg));
        memset(pArray,   0, sizeof(pArray));

        str_len = read(*sock, name_msg, sizeof(name_msg) - 1);
        if (str_len <= 0) { *sock = -1; return NULL; }

        name_msg[str_len] = '\0';
        name_msg[strcspn(name_msg, "\r")] = '\0';
        name_msg[strcspn(name_msg, "\n")] = '\0';
        strcpy(orig, name_msg);

        printf("[RECV] %s\n", orig);

        pToken = strtok(name_msg, "[:@]");
        i = 0;
        while (pToken != NULL) {
            pArray[i] = pToken;
            if (++i >= ARR_CNT) break;
            pToken = strtok(NULL, "[:@]");
        }
        if (i < 2) continue;

        /* AND -> LOG : Login request */
        if (!strcmp(pArray[0], AND_ID) && !strcmp(pArray[1], "LOGIN")) {
            if (i < 4) {
                printf("[LOGIN] insufficient args\n");
                snprintf(send_buf, sizeof(send_buf), "[%s]LOGIN@FAIL\n", AND_ID);
                write(*sock, send_buf, strlen(send_buf));
                continue;
            }
            strcpy(g_login_id,   pArray[2]);
            strcpy(g_login_name, pArray[3]);
            g_flow = FLOW_LOGIN;

            snprintf(send_buf, sizeof(send_buf),
                "[%s]GETDB@USERID@%s@UserName\n", SQL_ID, g_login_id);
            printf("[LOGIN] SQL query sent: %s\n", send_buf);
            write(*sock, send_buf, strlen(send_buf));
        }

        /* AND -> LOG : Register request */
        else if (!strcmp(pArray[0], AND_ID) && !strcmp(pArray[1], "CREATE")) {
            // 데이터 조각이 6개([AND], CREATE, ID, NAME, AGE, ALLERGY)이므로 조건을 i < 6으로 변경
            if (i < 6) {
                printf("[CREATE] insufficient args\n");
                snprintf(send_buf, sizeof(send_buf), "[%s]CREATE@FAIL\n", AND_ID);
                write(*sock, send_buf, strlen(send_buf));
                continue;
            }
            // 밀려있던 인덱스 번호들을 올바르게 교정 (3~6 -> 2~5)
            strcpy(g_reg_id,      pArray[2]);
            strcpy(g_reg_name,    pArray[3]);
            strcpy(g_reg_age,     pArray[4]);
            strcpy(g_reg_allergy, pArray[5]);
            g_flow = FLOW_REG_CHECK;

            snprintf(send_buf, sizeof(send_buf),
                "[%s]GETDB@USERID@%s\n", SQL_ID, g_reg_id);
            printf("[CREATE] duplicate check sent: %s\n", send_buf);
            write(*sock, send_buf, strlen(send_buf));
        }

        /* SQL -> LOG : GETDB response */
        else if (!strcmp(pArray[0], SQL_ID) && !strcmp(pArray[1], "GETDB")) {

            if (g_flow == FLOW_LOGIN) {
                int notfound = (i >= 4 &&
                    (!strcmp(pArray[3], "NOT_FOUND") || !strcmp(pArray[3], "FAIL")));

                if (notfound) {
                    printf("[LOGIN] ID not found: %s\n", pArray[2]);
                    snprintf(send_buf, sizeof(send_buf), "[%s]LOGIN@FAIL\n", AND_ID);
                }
                else if (i >= 5 && !strcmp(pArray[4], g_login_name)) {
                    printf("[LOGIN] success: %s\n", pArray[2]);
                    snprintf(send_buf, sizeof(send_buf),
                        "[%s]LOGIN@OK@%s@%s\n", AND_ID, pArray[2], pArray[4]);
                }
                else {
                    printf("[LOGIN] name mismatch: %s\n", pArray[2]);
                    snprintf(send_buf, sizeof(send_buf), "[%s]LOGIN@FAIL\n", AND_ID);
                }
                printf("[LOGIN] send to AND: %s\n", send_buf);
                write(*sock, send_buf, strlen(send_buf));
                g_flow = FLOW_NONE;
            }

            else if (g_flow == FLOW_REG_CHECK) {
                int notfound = (i >= 4 &&
                    (!strcmp(pArray[3], "NOT_FOUND") || !strcmp(pArray[3], "FAIL")));

                if (notfound) {
                    snprintf(send_buf, sizeof(send_buf),
                        "[%s]SETDB@%s@%s@%s@%s\n",
                        SQL_ID, g_reg_id, g_reg_name, g_reg_age, g_reg_allergy);
                    printf("[CREATE] SETDB sent: %s\n", send_buf);
                    write(*sock, send_buf, strlen(send_buf));
                    g_flow = FLOW_REG_INSERT;
                } else {
                    printf("[CREATE] duplicate ID: %s\n", pArray[2]);
                    snprintf(send_buf, sizeof(send_buf),
                        "[%s]CREATE@FAIL@ALREADY\n", AND_ID);
                    printf("[CREATE] send to AND: %s\n", send_buf);
                    write(*sock, send_buf, strlen(send_buf));
                    g_flow = FLOW_NONE;
                }
            }
        }

        /* SQL -> LOG : SETDB response */
        else if (!strcmp(pArray[0], SQL_ID) && !strcmp(pArray[1], "SETDB")) {
            if (g_flow == FLOW_REG_INSERT) {
                if (i >= 4 && !strcmp(pArray[3], "OK")) {
                    printf("[CREATE] register OK: %s\n", pArray[2]);
                    snprintf(send_buf, sizeof(send_buf),
                        "[%s]CREATE@OK@%s\n", AND_ID, pArray[2]);
                } else {
                    printf("[CREATE] register FAIL: %s\n", pArray[2]);
                    snprintf(send_buf, sizeof(send_buf), "[%s]CREATE@FAIL\n", AND_ID);
                }
                printf("[CREATE] send to AND: %s\n", send_buf);
                write(*sock, send_buf, strlen(send_buf));
                g_flow = FLOW_NONE;
            }
        }

        else {
            printf("[IGNORE] %s\n", orig);
        }
    }
}

void error_handling(char* msg)
{
    fputs(msg, stderr);
    fputc('\n', stderr);
    exit(1);
}
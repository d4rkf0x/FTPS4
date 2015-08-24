/*
 * Copyright (c) 2015 Sergi Granell (xerpi)
 */

#include "ps4.h"

extern int netdbg_sock;

#define UNUSED(x) (void)(x)


#define debug(...) \
	do { \
		char debug_buffer[512]; \
		int size = sprintf(debug_buffer, ##__VA_ARGS__); \
		sceNetSend(netdbg_sock, debug_buffer, size, 0); \
	} while(0)


#define DEBUG(...) debug(__VA_ARGS__)
#define INFO(...) debug(__VA_ARGS__)

#define PATH_MAX 255

#define NET_INIT_SIZE 1*1024*1024
#define FILE_BUF_SIZE 4*1024*1024

#define FTP_DEFAULT_PATH "/"

typedef enum {
	FTP_DATA_CONNECTION_NONE,
	FTP_DATA_CONNECTION_ACTIVE,
	FTP_DATA_CONNECTION_PASSIVE,
} DataConnectionType;

typedef struct ClientInfo {
	/* Client number */
	int num;
	/* Thread UID */
	ScePthread thid;
	/* Control connection socket FD */
	int ctrl_sockfd;
	/* Data connection attributes */
	int data_sockfd;
	DataConnectionType data_con_type;
	struct sockaddr_in data_sockaddr;
	/* PASV mode client socket */
	struct sockaddr_in pasv_sockaddr;
	int pasv_sockfd;
	/* Remote client net info */
	struct sockaddr_in addr;
	/* Receive buffer attributes */
	int n_recv;
	char recv_buffer[512];
	/* Current working directory */
	char cur_path[PATH_MAX];
	/* Rename path */
	char rename_path[PATH_MAX];
	/* Client list */
	struct ClientInfo *next;
	struct ClientInfo *prev;
} ClientInfo;

typedef void (*cmd_dispatch_func)(ClientInfo *client);

typedef struct {
	const char *cmd;
	cmd_dispatch_func func;
} cmd_dispatch_entry;

static void *net_memory = NULL;
static int ftp_initialized = 0;
static struct in_addr ps4_addr;
static unsigned short int ps4_port;
static ScePthread server_thid;
static int server_sockfd;
static int number_clients = 0;
static ClientInfo *client_list = NULL;
static ScePthreadMutex client_list_mtx;

#define client_send_ctrl_msg(cl, str) \
	sceNetSend(cl->ctrl_sockfd, str, strlen(str), 0)

static inline void client_send_data_msg(ClientInfo *client, const char *str)
{
	if (client->data_con_type == FTP_DATA_CONNECTION_ACTIVE) {
		sceNetSend(client->data_sockfd, str, strlen(str), 0);
	} else {
		sceNetSend(client->pasv_sockfd, str, strlen(str), 0);
	}
}

static inline int client_send_recv_raw(ClientInfo *client, void *buf, unsigned int len)
{
	if (client->data_con_type == FTP_DATA_CONNECTION_ACTIVE) {
		return sceNetRecv(client->data_sockfd, buf, len, 0);
	} else {
		return sceNetRecv(client->pasv_sockfd, buf, len, 0);
	}
}

static inline void client_send_data_raw(ClientInfo *client, const void *buf, unsigned int len)
{
	if (client->data_con_type == FTP_DATA_CONNECTION_ACTIVE) {
		sceNetSend(client->data_sockfd, buf, len, 0);
	} else {
		sceNetSend(client->pasv_sockfd, buf, len, 0);
	}
}

static void cmd_USER_func(ClientInfo *client)
{
	client_send_ctrl_msg(client, "331 Username OK, need password b0ss.\n");
}

static void cmd_PASS_func(ClientInfo *client)
{
	client_send_ctrl_msg(client, "230 User logged in!\n");
}

static void cmd_QUIT_func(ClientInfo *client)
{
	client_send_ctrl_msg(client, "221 Goodbye senpai :'(\n");
}

static void cmd_SYST_func(ClientInfo *client)
{
	client_send_ctrl_msg(client, "215 UNIX Type: L8\n");
}

static void cmd_PASV_func(ClientInfo *client)
{
	int ret;
	UNUSED(ret);

	char cmd[512];
	unsigned int namelen;
	struct sockaddr_in picked;

	/* Create data mode socket name */
	char data_socket_name[64];
	sprintf(data_socket_name, "FTPS4_client_%i_data_socket",
		client->num);

	/* Create the data socket */
	client->data_sockfd = sceNetSocket(data_socket_name,
		AF_INET,
		SOCK_STREAM,
		0);

	DEBUG("PASV data socket fd: %d\n", client->data_sockfd);

	/* Fill the data socket address */
	client->data_sockaddr.sin_family = sceNetHtons(AF_INET);
	client->data_sockaddr.sin_addr.s_addr = sceNetHtonl(IN_ADDR_ANY);
	/* Let the PS4 choose a port */
	client->data_sockaddr.sin_port = sceNetHtons(0);

	/* Bind the data socket address to the data socket */
	ret = sceNetBind(client->data_sockfd,
		(struct sockaddr *)&client->data_sockaddr,
		sizeof(client->data_sockaddr));
	DEBUG("sceNetBind(): 0x%08X\n", ret);

	/* Start listening */
	ret = sceNetListen(client->data_sockfd, 128);
	DEBUG("sceNetListen(): 0x%08X\n", ret);

	/* Get the port that the PS4 has chosen */
	namelen = sizeof(picked);
	sceNetGetsockname(client->data_sockfd, (struct sockaddr *)&picked,
		&namelen);

	DEBUG("PASV mode port: 0x%04X\n", picked.sin_port);

	/* Build the command */
	sprintf(cmd, "227 Entering Passive Mode (%hhu,%hhu,%hhu,%hhu,%hhu,%hhu)\n",
		(ps4_addr.s_addr >> 0) & 0xFF,
		(ps4_addr.s_addr >> 8) & 0xFF,
		(ps4_addr.s_addr >> 16) & 0xFF,
		(ps4_addr.s_addr >> 24) & 0xFF,
		(picked.sin_port >> 0) & 0xFF,
		(picked.sin_port >> 8) & 0xFF);

	client_send_ctrl_msg(client, cmd);

	/* Set the data connection type to passive! */
	client->data_con_type = FTP_DATA_CONNECTION_PASSIVE;
}

static void cmd_PORT_func(ClientInfo *client)
{
	unsigned char data_ip[4];
	unsigned char porthi, portlo;
	unsigned short data_port;
	char ip_str[16];
	struct in_addr data_addr;

	sscanf(client->recv_buffer, "%*s %hhu,%hhu,%hhu,%hhu,%hhu,%hhu",
		&data_ip[0], &data_ip[1], &data_ip[2], &data_ip[3],
		&porthi, &portlo);

	data_port = portlo + porthi*256;

	/* Convert to an X.X.X.X IP string */
	sprintf(ip_str, "%d.%d.%d.%d",
		data_ip[0], data_ip[1], data_ip[2], data_ip[3]);

	/* Convert the IP to a struct in_addr */
	sceNetInetPton(AF_INET, ip_str, &data_addr);

	DEBUG("PORT connection to client's IP: %s Port: %d\n", ip_str, data_port);

	/* Create data mode socket name */
	char data_socket_name[64];
	sprintf(data_socket_name, "FTPS4_client_%i_data_socket",
		client->num);

	/* Create data mode socket */
	client->data_sockfd = sceNetSocket(data_socket_name,
		AF_INET,
		SOCK_STREAM,
		0);

	DEBUG("Client %i data socket fd: %d\n", client->num,
		client->data_sockfd);

	/* Prepare socket address for the data connection */
	client->data_sockaddr.sin_family = sceNetHtons(AF_INET);
	client->data_sockaddr.sin_addr = data_addr;
	client->data_sockaddr.sin_port = sceNetHtons(data_port);

	/* Set the data connection type to active! */
	client->data_con_type = FTP_DATA_CONNECTION_ACTIVE;

	client_send_ctrl_msg(client, "200 PORT command successful!\n");
}

static void client_open_data_connection(ClientInfo *client)
{
	int ret;
	UNUSED(ret);

	unsigned int addrlen;

	if (client->data_con_type == FTP_DATA_CONNECTION_ACTIVE) {
		/* Connect to the client using the data socket */
		ret = sceNetConnect(client->data_sockfd,
			(struct sockaddr *)&client->data_sockaddr,
			sizeof(client->data_sockaddr));

		DEBUG("sceNetConnect(): 0x%08X\n", ret);
	} else {
		/* Listen to the client using the data socket */
		addrlen = sizeof(client->pasv_sockaddr);
		client->pasv_sockfd = sceNetAccept(client->data_sockfd,
			(struct sockaddr *)&client->pasv_sockaddr,
			&addrlen);
		DEBUG("PASV client fd: 0x%08X\n", client->pasv_sockfd);
	}
}

static void client_close_data_connection(ClientInfo *client)
{
	sceNetSocketClose(client->data_sockfd);
	/* In passive mode we have to close the client pasv socket too */
	if (client->data_con_type == FTP_DATA_CONNECTION_PASSIVE) {
		sceNetSocketClose(client->pasv_sockfd);
	}
	client->data_con_type = FTP_DATA_CONNECTION_NONE;
}

static int gen_list_format(char *out, int n, int dir, unsigned int file_size,
	int month_n, int day_n, int hour, int minute, const char *filename)
{
	static const char num_to_month[][4] = {
		"Jan", "Feb", "Mar", "Apr", "May", "Jun",
		"Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
	};

	return snprintf(out, n,
		"%c%s 1 ps4 ps4 %d %s %-2d %02d:%02d %s\r\n",
		dir ? 'd' : '-',
		dir ? "rwxr-xr-x" : "rw-r--r--",
		file_size,
		num_to_month[month_n%12],
		day_n,
		hour,
		minute,
		filename);
}

static void send_LIST(ClientInfo *client, const char *path)
{
	char buffer[512];
	int dfd;
	struct dirent *dent;
	char dentbuf[512];
	struct stat st;

	dfd = open(path, O_RDONLY, 0);
	if (dfd < 0) {
		client_send_ctrl_msg(client, "550 Invalid directory.\n");
		return;
	}

	client_send_ctrl_msg(client, "150 Opening ASCII mode data transfer for LIST.\n");

	client_open_data_connection(client);

	while (memset(dentbuf, 0, sizeof(dentbuf)), getdents(dfd, dentbuf, sizeof(dentbuf)) != 0) {
		dent = (struct dirent *)dentbuf;

		while (dent->d_reclen) {
			if(dent->d_type == DT_REG) {
				/*stat(dent->d_name, &st);

				struct tm tm;
				gmtime_r(&st.st_ctim.tv_sec, &tm);

				gen_list_format(buffer, sizeof(buffer),
					dent->d_type == DT_DIR,
					st.st_size,
					tm.tm_mon,
					tm.tm_mday,
					tm.tm_hour,
					tm.tm_min,
					dent->d_name);*/
				
				sprintf(buffer, ".rw-r--r-- 1 xerpi users 1025 Aug 23 16:54 %s\r\n", dent->d_name);
			}
			else/* if(dent->d_type == DT_DIR)*/ {
				/*gen_list_format(buffer, sizeof(buffer),
					1,
					4096,
					0,
					0,
					0,
					0,
					dent->d_name);*/
				
				sprintf(buffer, "drw-r--r-- 1 xerpi users 1025 Aug 23 16:54 %s\r\n", dent->d_name);
			}
			//else {
			//	sprintf(buffer, "-rw-r--r-- 1 xerpi users 1025 Aug 23 16:54 %s\r\n", dent->d_name);
			//}

			DEBUG(buffer);
			
			client_send_data_msg(client, buffer);
            memset(buffer, 0, sizeof(buffer));
			
			dent = (struct dirent *)((void *)dent + dent->d_reclen);
		}
	}

	close(dfd);

	DEBUG("Done sending LIST\n");

	client_close_data_connection(client);
	client_send_ctrl_msg(client, "226 Transfer complete.\n");
}

static void cmd_LIST_func(ClientInfo *client)
{
	char list_path[PATH_MAX];

	int n = sscanf(client->recv_buffer, "%*s %[^\r\n\t]", list_path);

	if (n > 0) {  /* Client specified a path */
		send_LIST(client, list_path);
	} else {      /* Use current path */
		send_LIST(client, client->cur_path);
	}
}

static void cmd_PWD_func(ClientInfo *client)
{
	char msg[PATH_MAX];
	/* We don't want to send "cache0:" */
	const char *pwd_path = strchr(client->cur_path, '/');

	sprintf(msg, "257 \"%s\" is the current directory.\n", pwd_path);
	client_send_ctrl_msg(client, msg);
}

static void cmd_CWD_func(ClientInfo *client)
{
	char cmd_path[PATH_MAX];
	char path[PATH_MAX];
	int pd;
	int n = sscanf(client->recv_buffer, "%*s %[^\r\n\t]", cmd_path);

	if (n < 1) {
		client_send_ctrl_msg(client, "500 Syntax error, command unrecognized.\n");
	} else {
		if (cmd_path[0] != '/') { /* Change dir relative to current dir */
			sprintf(path, "%s%s", client->cur_path, cmd_path);
		} else {
			sprintf(path, "cache0:%s", cmd_path);
		}

		/* If there isn't "/" at the end, add it */
		if (path[strlen(path) - 1] != '/') {
			strcat(path, "/");
		}

		/* Check if the path exists */
		pd = open(path, O_RDONLY, 0);
		if (pd < 0) {
			client_send_ctrl_msg(client, "550 Invalid directory.\n");
			return;
		}
		close(pd);

		strcpy(client->cur_path, path);
		client_send_ctrl_msg(client, "250 Requested file action okay, completed.\n");
	}
}

static void cmd_TYPE_func(ClientInfo *client)
{
	char data_type;
	char format_control[8];
	int n_args = sscanf(client->recv_buffer, "%*s %c %s", &data_type, format_control);

	if (n_args > 0) {
		switch(data_type) {
		case 'A':
		case 'I':
			client_send_ctrl_msg(client, "200 Okay\n");
			break;
		case 'E':
		case 'L':
		default:
			client_send_ctrl_msg(client, "504 Error: bad parameters?\n");
			break;
		}
	} else {
		client_send_ctrl_msg(client, "504 Error: bad parameters?\n");
	}
}

static void dir_up(char *path)
{
	char *pch;
	size_t len_in = strlen(path);
	if (len_in == 1) {
		strcpy(path, "/");
		return;
	}
	if (path[len_in - 1] == '/') {
		path[len_in - 1] = '\0';
	}
	pch = strrchr(path, '/');
	if (pch) {
		size_t s = len_in - (pch - path);
		memset(pch + 1, '\0', s);
	}
}

static void cmd_CDUP_func(ClientInfo *client)
{
	char path[PATH_MAX];
	/* Path without "cache0:" */
	const char *normal_path = strchr(client->cur_path, '/');
	strcpy(path, normal_path);
	dir_up(path);
	sprintf(client->cur_path, "cache0:%s", path);
	client_send_ctrl_msg(client, "200 Command okay.\n");
}

static void send_file(ClientInfo *client, const char *path)
{
	unsigned char *buffer;
	int fd;
	unsigned int bytes_read;

	DEBUG("Opening: %s\n", path);

	if ((fd = open(path, O_RDONLY, 0)) >= 0) {

		buffer = malloc(FILE_BUF_SIZE);
		if (buffer == NULL) {
			client_send_ctrl_msg(client, "550 Could not allocate memory.\n");
			return;
		}

		client_open_data_connection(client);
		client_send_ctrl_msg(client, "150 Opening Image mode data transfer.\n");

		while ((bytes_read = read(fd, buffer, FILE_BUF_SIZE)) > 0) {
			client_send_data_raw(client, buffer, bytes_read);
		}

		close(fd);
		free(buffer);
		client_send_ctrl_msg(client, "226 Transfer completed.\n");
		client_close_data_connection(client);

	} else {
		client_send_ctrl_msg(client, "550 File not found.\n");
	}
}

/* This function generates a PS4 valid path with the input path
 * from RETR, STOR, DELE, RMD, MKD, RNFR and RNTO commands */
static void gen_filepath(ClientInfo *client, char *dest_path)
{
	char cmd_path[PATH_MAX];
	sscanf(client->recv_buffer, "%*[^ ] %[^\r\n\t]", cmd_path);

	if (cmd_path[0] != '/') { /* The file is relative to current dir */
		/* Append the file to the current path */
		sprintf(dest_path, "%s%s", client->cur_path, cmd_path);
	} else {
		/* Add "cache0:" to the file */
		sprintf(dest_path, "cache0:%s", cmd_path);
	}
}

static void cmd_RETR_func(ClientInfo *client)
{
	char dest_path[PATH_MAX];
	gen_filepath(client, dest_path);
	send_file(client, dest_path);
}

static void receive_file(ClientInfo *client, const char *path)
{
	unsigned char *buffer;
	int fd;
	unsigned int bytes_recv;

	DEBUG("Opening: %s\n", path);

	if ((fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0)) >= 0) {

		buffer = malloc(FILE_BUF_SIZE);
		if (buffer == NULL) {
			client_send_ctrl_msg(client, "550 Could not allocate memory.\n");
			return;
		}

		client_open_data_connection(client);
		client_send_ctrl_msg(client, "150 Opening Image mode data transfer.\n");

		while ((bytes_recv = client_send_recv_raw(client, buffer, FILE_BUF_SIZE)) > 0) {
			write(fd, buffer, bytes_recv);
		}

		close(fd);
		free(buffer);
		client_send_ctrl_msg(client, "226 Transfer completed.\n");
		client_close_data_connection(client);

	} else {
		client_send_ctrl_msg(client, "550 File not found.\n");
	}
}

static void cmd_STOR_func(ClientInfo *client)
{
	char dest_path[PATH_MAX];
	gen_filepath(client, dest_path);
	receive_file(client, dest_path);
}

static void delete_file(ClientInfo *client, const char *path)
{
	DEBUG("Deleting: %s\n", path);

	if (unlink(path) >= 0) {
		client_send_ctrl_msg(client, "226 File deleted.\n");
	} else {
		client_send_ctrl_msg(client, "550 Could not delete the file.\n");
	}
}

static void cmd_DELE_func(ClientInfo *client)
{
	char dest_path[PATH_MAX];
	gen_filepath(client, dest_path);
	delete_file(client, dest_path);
}

static void delete_dir(ClientInfo *client, const char *path)
{
	int ret;
	DEBUG("Deleting: %s\n", path);
	ret = rmdir(path);
	if (ret >= 0) {
		client_send_ctrl_msg(client, "226 Directory deleted.\n");
	} else if (ret == 0x8001005A) { /* DIRECTORY_IS_NOT_EMPTY */
		client_send_ctrl_msg(client, "550 Directory is not empty.\n");
	} else {
		client_send_ctrl_msg(client, "550 Could not delete the directory.\n");
	}
}

static void cmd_RMD_func(ClientInfo *client)
{
	char dest_path[PATH_MAX];
	gen_filepath(client, dest_path);
	delete_dir(client, dest_path);
}

static void create_dir(ClientInfo *client, const char *path)
{
	DEBUG("Creating: %s\n", path);

	if (mkdir(path, 0777) >= 0) {
		client_send_ctrl_msg(client, "226 Directory created.\n");
	} else {
		client_send_ctrl_msg(client, "550 Could not create the directory.\n");
	}
}

static void cmd_MKD_func(ClientInfo *client)
{
	char dest_path[PATH_MAX];
	gen_filepath(client, dest_path);
	create_dir(client, dest_path);
}

static int file_exists(const char *path)
{
	struct stat s;
	return (stat(path, &s) >= 0);
}

static void cmd_RNFR_func(ClientInfo *client)
{
	char from_path[PATH_MAX];
	/* Get the origin filename */
	gen_filepath(client, from_path);

	/* Check if the file exists */
	if (!file_exists(from_path)) {
		client_send_ctrl_msg(client, "550 The file doesn't exist.\n");
		return;
	}
	/* The file to be renamed is the received path */
	strcpy(client->rename_path, from_path);
	client_send_ctrl_msg(client, "250 I need the destination name b0ss.\n");
}

static void cmd_RNTO_func(ClientInfo *client)
{
	char path_to[PATH_MAX];
	/* Get the destination filename */
	gen_filepath(client, path_to);

	DEBUG("Renaming: %s to %s\n", client->rename_path, path_to);

	if (rename(client->rename_path, path_to) < 0) {
		client_send_ctrl_msg(client, "550 Error renaming the file.\n");
	}

	client_send_ctrl_msg(client, "226 Rename completed.\n");
}

static void cmd_SIZE_func(ClientInfo *client)
{
	struct stat s;
	char path[PATH_MAX];
	char cmd[64];
	/* Get the filename to retrieve its size */
	gen_filepath(client, path);

	/* Check if the file exists */
	if (stat(path, &s) < 0) {
		client_send_ctrl_msg(client, "550 The file doesn't exist.\n");
		return;
	}
	/* Send the size of the file */
	sprintf(cmd, "213: %lld\n", s.st_size);
	client_send_ctrl_msg(client, cmd);
}

#define add_entry(name) {#name, cmd_##name##_func}
static const cmd_dispatch_entry cmd_dispatch_table[] = {
	add_entry(USER),
	add_entry(PASS),
	add_entry(QUIT),
	add_entry(SYST),
	add_entry(PASV),
	add_entry(PORT),
	add_entry(LIST),
	add_entry(PWD),
	add_entry(CWD),
	add_entry(TYPE),
	add_entry(CDUP),
	add_entry(RETR),
	add_entry(STOR),
	add_entry(DELE),
	add_entry(RMD),
	add_entry(MKD),
	add_entry(RNFR),
	add_entry(RNTO),
	add_entry(SIZE),
	{NULL, NULL}
};

static cmd_dispatch_func get_dispatch_func(const char *cmd)
{
	int i;
	for(i = 0; cmd_dispatch_table[i].cmd && cmd_dispatch_table[i].func; i++) {
		if (strcmp(cmd, cmd_dispatch_table[i].cmd) == 0) {
			return cmd_dispatch_table[i].func;
		}
	}
	return NULL;
}

static void client_list_add(ClientInfo *client)
{
	/* Add the client at the front of the client list */
	scePthreadMutexLock(&client_list_mtx);

	if (client_list == NULL) { /* List is empty */
		client_list = client;
		client->prev = NULL;
		client->next = NULL;
	} else {
		client->next = client_list;
		client->next->prev = client;
		client->prev = NULL;
		client_list = client;
	}

	scePthreadMutexUnlock(&client_list_mtx);
}

static void client_list_delete(ClientInfo *client)
{
	/* Remove the client from the client list */
	scePthreadMutexLock(&client_list_mtx);

	if (client->prev) {
		client->prev->next = client->next;
	}
	if (client->next) {
		client->next->prev = client->prev;
	}

	scePthreadMutexUnlock(&client_list_mtx);
}

static void client_list_close_sockets()
{
	/* Iterate over the client list and close their sockets */
	scePthreadMutexLock(&client_list_mtx);

	ClientInfo *it = client_list;

	while (it) {
		sceNetSocketClose(it->ctrl_sockfd);
		it = it->next;
	}

	scePthreadMutexUnlock(&client_list_mtx);
}

static void *client_thread(void *arg)
{
	char cmd[16];
	cmd_dispatch_func dispatch_func;
	ClientInfo *client = (ClientInfo *)arg;

	DEBUG("Client thread %i started!\n", client->num);

	client_send_ctrl_msg(client, "220 FTPS4 Server ready.\n");

	while (1) {

		memset(client->recv_buffer, 0, sizeof(client->recv_buffer));

		client->n_recv = sceNetRecv(client->ctrl_sockfd, client->recv_buffer, sizeof(client->recv_buffer), 0);
		if (client->n_recv > 0) {
			DEBUG("Received %i bytes from client number %i:\n",
				client->n_recv, client->num);

			INFO("\t%i> %s", client->num, client->recv_buffer);

			/* The command are the first chars until the first space */
			sscanf(client->recv_buffer, "%s", cmd);

			/* Wait 1 ms before sending any data */
			sceKernelUsleep(1*1000);

			if ((dispatch_func = get_dispatch_func(cmd))) {
				dispatch_func(client);
			} else {
				client_send_ctrl_msg(client, "502 Sorry, command not implemented. :(\n");
			}

		} else if (client->n_recv == 0) {
			/* Value 0 means connection closed by the remote peer */
			INFO("Connection closed by the client %i.\n", client->num);
			/* Close the client's socket */
			sceNetSocketClose(client->ctrl_sockfd);
			/* Delete itself from the client list */
			client_list_delete(client);
			break;
		} else {
			/* A negative value means error */
			break;
		}
	}

	/* If there's an open data connection, close it */
	if (client->data_con_type != FTP_DATA_CONNECTION_NONE) {
		sceNetSocketClose(client->data_sockfd);
		if (client->data_con_type == FTP_DATA_CONNECTION_PASSIVE) {
			sceNetSocketClose(client->pasv_sockfd);
		}
	}

	DEBUG("Client thread %i exiting!\n", client->num);

	free(client);

	scePthreadExit(NULL);
	return NULL;
}

static void *server_thread(void *arg)
{
	int ret;
	UNUSED(ret);

	struct sockaddr_in serveraddr;

	DEBUG("Server thread started!\n");

	/* Create server socket */
	server_sockfd = sceNetSocket("FTPS4_server_sock",
		AF_INET,
		SOCK_STREAM,
		0);

	DEBUG("Server socket fd: %d\n", server_sockfd);

	/* Fill the server's address */
	serveraddr.sin_family = sceNetHtons(AF_INET);
	serveraddr.sin_addr.s_addr = sceNetHtonl(IN_ADDR_ANY);
	serveraddr.sin_port = sceNetHtons(ps4_port);

	/* Bind the server's address to the socket */
	ret = sceNetBind(server_sockfd, (struct sockaddr *)&serveraddr, sizeof(serveraddr));
	DEBUG("sceNetBind(): 0x%08X\n", ret);

	/* Start listening */
	ret = sceNetListen(server_sockfd, 128);
	DEBUG("sceNetListen(): 0x%08X\n", ret);

	while (1) {

		/* Accept clients */
		struct sockaddr_in clientaddr;
		int client_sockfd;
		unsigned int addrlen = sizeof(clientaddr);

		DEBUG("Waiting for incoming connections...\n");

		client_sockfd = sceNetAccept(server_sockfd, (struct sockaddr *)&clientaddr, &addrlen);
		if (client_sockfd >= 0) {

			DEBUG("New connection, client fd: 0x%08X\n", client_sockfd);

			/* Get the client's IP address */
			char remote_ip[16];
			sceNetInetNtop(AF_INET,
				&clientaddr.sin_addr.s_addr,
				remote_ip,
				sizeof(remote_ip));

			INFO("Client %i connected, IP: %s port: %i\n",
				number_clients, remote_ip, clientaddr.sin_port);

			/* Allocate the ClientInfo struct for the new client */
			ClientInfo *client = malloc(sizeof(*client));
			client->num = number_clients;
			client->ctrl_sockfd = client_sockfd;
			client->data_con_type = FTP_DATA_CONNECTION_NONE;
			strcpy(client->cur_path, FTP_DEFAULT_PATH);
			memcpy(&client->addr, &clientaddr, sizeof(client->addr));

			/* Add the new client to the client list */
			client_list_add(client);

			/* Create a new thread for the client */
			char client_thread_name[64];
			sprintf(client_thread_name, "FTPS4_client_%i_thread",
				number_clients);

			/* Create a new thread for the client */
			scePthreadCreate(&client->thid, NULL, client_thread, client, client_thread_name);

			DEBUG("Client %i thread UID: 0x%08X\n", number_clients, client->thid);

			number_clients++;
		} else {
			/* if sceNetAccept returns < 0, it means that the listening
			 * socket has been closed, this means that we want to
			 * finish the server thread */
			DEBUG("Server socket closed, 0x%08X\n", client_sockfd);
			break;
		}
	}

	DEBUG("Server thread exiting!\n");

	scePthreadExit(NULL);
	return NULL;
}

void ftp_init(const char *ip, unsigned short int port)
{
	int ret;
	UNUSED(ret);

	//SceNetInitParam initparam;
	//SceNetCtlInfo info;

	if (ftp_initialized) {
		return;
	}

	/* Init Net */
	/*if (sceNetShowNetstat() == PSP2_NET_ERROR_ENOTINIT) {
		net_memory = malloc(NET_INIT_SIZE);

		initparam.memory = net_memory;
		initparam.size = NET_INIT_SIZE;
		initparam.flags = 0;

		ret = sceNetInit(&initparam);
		DEBUG("sceNetInit(): 0x%08X\n", ret);
	} else {
		DEBUG("Net is already initialized.\n");
	}*/

	/* Init NetCtl */
	//ret = sceNetCtlInit();
	//DEBUG("sceNetCtlInit(): 0x%08X\n", ret);

	/* Get IP address */
	//ret = sceNetCtlInetGetInfo(PSP2_NETCTL_INFO_GET_IP_ADDRESS, &info);
	//DEBUG("sceNetCtlInetGetInfo(): 0x%08X\n", ret);

	/* Return data */
	//strcpy(vita_ip, info.ip_address);
	//*vita_port = FTP_PORT;

	/* Save the listening port of the PS4 to a global variable */
	ps4_port = port;

	/* Save the IP of the PS4 to a global variable */
	sceNetInetPton(AF_INET, ip, &ps4_addr);

	/* Create the client list mutex */
	scePthreadMutexInit(&client_list_mtx, NULL, "FTPS4_client_list_mutex");
	DEBUG("Client list mutex UID: 0x%08X\n", client_list_mtx);

	/* Create server thread */
	scePthreadCreate(&server_thid, NULL, server_thread, NULL, "FTPS4_server_thread");
	DEBUG("Server thread UID: 0x%08X\n", server_thid);

	ftp_initialized = 1;
}

void ftp_fini()
{
	if (ftp_initialized) {
		/* In order to "stop" the blocking sceNetAccept,
		 * we have to close the server socket; this way
		 * the accept call will return an error */
		sceNetSocketClose(server_sockfd);

		/* To close the clients we have to do the same:
		 * we have to iterate over all the clients
		 * and close their sockets */
		client_list_close_sockets();
		client_list = NULL;

		/* UGLY: Give 50 ms for the threads to exit */
		sceKernelUsleep(50*1000);

		/* Delete the client list mutex */
		scePthreadMutexDestroy(client_list_mtx);

		//sceNetCtlTerm();
		//sceNetTerm();

		if (net_memory) {
			free(net_memory);
			net_memory = NULL;
		}

		ftp_initialized = 0;
	}
}


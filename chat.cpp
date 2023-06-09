#include <curses.h>
#include <readline/history.h>
#include <readline/readline.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/err.h>
#include <string.h>
#include <getopt.h>
#include <string>
using std::string;
#include <deque>
using std::deque;
#include <pthread.h>
#include <utility>
using std::pair;
#include "dh.h"

#define HOST_NAME_MAX 128

static pthread_t trecv;     /* wait for incoming messagess and post to queue */
void* recvMsg(void*);       /* for trecv */
static pthread_t tcurses;   /* setup curses and draw messages from queue */
void* cursesthread(void*);  /* for tcurses */
/* tcurses will get a queue full of these and redraw the appropriate windows */
struct redraw_data {
	bool resize;
	string msg;
	string sender;
	WINDOW* win;
};
static deque<redraw_data> mq; /* messages and resizes yet to be drawn */
/* manage access to message queue: */
static pthread_mutex_t qmx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t qcv = PTHREAD_COND_INITIALIZER;

/* XXX different colors for different senders */

/* record chat history as deque of strings: */
static deque<string> transcript;

#define max(a, b)         \
	({ typeof(a) _a = a;    \
	 typeof(b) _b = b;    \
	 _a > _b ? _a : _b; })

/* network stuff... */

int listensock, sockfd;

/* Session Keys */
unsigned char* client_key;
unsigned char* server_key;
int message_length;

[[noreturn]] static void fail_exit(const char *msg);

[[noreturn]] static void error(const char *msg)
{
	perror(msg);
	fail_exit("");
}

int initServerNet(int port)
{
	int reuse = 1;
	struct sockaddr_in serv_addr;
	listensock = socket(AF_INET, SOCK_STREAM, 0);
	setsockopt(listensock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
	/* NOTE: might not need the above if you make sure the client closes first */
	if (listensock < 0)
		error("ERROR opening socket");
	bzero((char *) &serv_addr, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = INADDR_ANY;
	serv_addr.sin_port = htons(port);
	if (bind(listensock, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0)
		error("ERROR on binding");
	fprintf(stderr, "listening on port %i...\n",port);
	listen(listensock,1);
	socklen_t clilen;
	struct sockaddr_in  cli_addr;
	sockfd = accept(listensock, (struct sockaddr *) &cli_addr, &clilen);
	if (sockfd < 0)
		error("error on accept");
	close(listensock);
	fprintf(stderr, "connection made, starting session...\n");
	/* at this point, should be able to send/recv on sockfd */
	
	// Generate key
	
	if (init("params") == 0) {
		// gmp_printf("Successfully read DH params:\nq = %Zd\np = %Zd\ng = %Zd\n",q,p,g);
	}
	/* Alice: */
	NEWZ(a); /* secret key (a random exponent) */
	NEWZ(A); /* public key: A = g^a mod p */
	dhGen(a,A);
	//gmp_printf("A: %Zd\n", A);
	
	NEWZ(x); /* secret key (a random exponent) */
	NEWZ(X); /* public key: A = g^a mod p */
	dhGen(x,X);
	gmp_printf("X: %Zd\n", X);
	

	mpz_t Y;
	mpz_init(Y);
	// receive public key from clientS
	size_t lenY;
	recv(sockfd, (void*)&lenY, sizeof(size_t), 0);
	char* strY = (char*) malloc(sizeof(char) * (lenY + 1));
	recv(sockfd, strY, lenY, 0);
	mpz_set_str(Y, strY, 10);
	free(strY);
	//gmp_printf("Y: %Zd\n", Y);
	// recv(sockfd, &B, sizeof(mpz_t), 0);
	// send public key to client
	char* strX = (char*) malloc(sizeof(char) * (mpz_sizeinbase(X, 10) + 1));
	mpz_get_str(strX, 10, X);
	size_t lenX = strlen(strX);
	send(sockfd, (void*)&lenX, sizeof(size_t), 0);
	send(sockfd, strX, lenX, 0);
	free(strX);
	// send(sockfd, &A, sizeof(mpz_t), 0);

	// const size_t klen = 32;
	const size_t klen = 128;
	/* Alice's key derivation: */
	unsigned char kA[klen];
	dhFinal(a,A,Y,kA,klen);
	//printf("Alice's key:\n");
	//for (size_t i = 0; i < klen; i++) {
	//	printf("%02x ",kA[i]);
	//}
	
	unsigned char kC[klen];
	dhFinal(x,X,Y,kC,klen);
	//printf("\nThis is what Alice Sees:\n");
	for (size_t i = 0; i < klen; i++) {
	//	printf("%02x ",kC[i]);
	//	IDK DOES NOT WORK WITHOUT LOOP
	}
	//printf("\n");
	server_key = kC;
	// End of generating key
	
	//printf("\nSaved Server Key:\n");
	//for (size_t i = 0; i < klen; i++) {
	//	printf("%02x ",server_key[i]);
	//}
	//printf("\n");

	FILE *fptr;
	fptr = fopen("server_key.bin", "w");
	fwrite(kC, sizeof(unsigned char), klen, fptr);
	fclose(fptr);

	return 0;
}


static int initClientNet(char* hostname, int port)
{
	struct sockaddr_in serv_addr;
	sockfd = socket(AF_INET, SOCK_STREAM, 0);
	struct hostent *server;
	if (sockfd < 0)
		error("ERROR opening socket");
	server = gethostbyname(hostname);
	if (server == NULL) {
		fprintf(stderr,"ERROR, no such host\n");
		exit(0);
	}
	bzero((char *) &serv_addr, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	memcpy(&serv_addr.sin_addr.s_addr,server->h_addr,server->h_length);
	serv_addr.sin_port = htons(port);
	if (connect(sockfd,(struct sockaddr *) &serv_addr,sizeof(serv_addr)) < 0)
		error("ERROR connecting");
	/* at this point, should be able to send/recv on sockfd */

	// Generate key
	if (init("params") == 0) {
		// gmp_printf("Successfully read DH params:\nq = %Zd\np = %Zd\ng = %Zd\n",q,p,g);
	}
	/* Bob: */
	NEWZ(b); /* secret key (a random exponent) */
	NEWZ(B); /* public key: B = g^b mod p */
	dhGen(b,B);
	
	NEWZ(y); /* secret key (a random exponent) */
	NEWZ(Y); /* public key: A = g^a mod p */
	dhGen(y,Y);
	//gmp_printf("Y: %Zd\n", Y);

	//gmp_printf("B: %Zd\n", B);
	// send public key to server
	char* strY = (char*) malloc(sizeof(char) * (mpz_sizeinbase(Y, 10) + 1));
	mpz_get_str(strY, 10, Y);
	size_t lenY = strlen(strY);
	send(sockfd, (void*)&lenY, sizeof(size_t), 0);
	send(sockfd, strY, lenY, 0);
	free(strY);
	// send(sockfd, &B, sizeof(mpz_t), 0);
	
	mpz_t X;
	mpz_init(X);
	// receive public key from server
	size_t lenX;
	recv(sockfd, (void*)&lenX, sizeof(size_t), 0);
	char* strX = (char*) malloc(sizeof(char) * (lenX + 1));
	recv(sockfd, strX, lenX, 0);
	mpz_set_str(X, strX, 10);
	free(strX);
	//gmp_printf("X: %Zd\n", X);
	// recv(sockfd, &A, sizeof(mpz_t), 0);

	// const size_t klen = 32;
	const size_t klen = 128;
	/* Bob's key derivation: */
	unsigned char kB[klen];
	dhFinal(b,B,X,kB,klen);
	//printf("Bob's key:\n");
	
	//for (size_t i = 0; i < klen; i++) {
	//	printf("%02x ",kB[i]);
	//}
	
	unsigned char kC[klen];
	dhFinal(y,Y,X,kC,klen);
	//printf("\nThis is what Bob Sees:\n");
	//for (size_t i = 0; i < klen; i++) {
	//	printf("%02x ",kC[i]);
	//}

	client_key = kC;
	//printf("\nSaved Client Key:\n");
	//for (size_t i = 0; i < klen; i++) {
	//	printf("%02x ",client_key[i]);
	//}
	//printf("\n");
	
	// End of generating key

	FILE *fptr;
	fptr = fopen("client_key.bin", "w");
	fwrite(kC, sizeof(unsigned char), klen, fptr);
	fclose(fptr);

	return 0;
}

static int shutdownNetwork()
{
	shutdown(sockfd,2);
	unsigned char dummy[64];
	ssize_t r;
	do {
		r = recv(sockfd,dummy,64,0);
	} while (r != 0 && r != -1);
	close(sockfd);
	return 0;
}

/* end network stuff. */


[[noreturn]] static void fail_exit(const char *msg)
{
	// Make sure endwin() is only called in visual mode. As a note, calling it
	// twice does not seem to be supported and messed with the cursor position.
	if (!isendwin())
		endwin();
	fprintf(stderr, "%s\n", msg);
	exit(EXIT_FAILURE);
}

// Checks errors for (most) ncurses functions. CHECK(fn, x, y, z) is a checked
// version of fn(x, y, z).
#define CHECK(fn, ...) \
	do \
	if (fn(__VA_ARGS__) == ERR) \
	fail_exit(#fn"("#__VA_ARGS__") failed"); \
	while (false)

static bool should_exit = false;

// Message window
static WINDOW *msg_win;
// Separator line above the command (readline) window
static WINDOW *sep_win;
// Command (readline) window
static WINDOW *cmd_win;

// Input character for readline
static unsigned char input;

static int readline_getc(FILE *dummy)
{
	return input;
}

/* if batch is set, don't draw immediately to real screen (use wnoutrefresh
 * instead of wrefresh) */
static void msg_win_redisplay(bool batch, const string& newmsg="", const string& sender="")
{
	if (batch)
		wnoutrefresh(msg_win);
	else {
		wattron(msg_win,COLOR_PAIR(2));
		wprintw(msg_win,"%s:",sender.c_str());
		wattroff(msg_win,COLOR_PAIR(2));
		wprintw(msg_win," %s\n",newmsg.c_str());
		wrefresh(msg_win);
	}
}

static void msg_typed(char *line)
{
	string mymsg;
	if (!line) {
		// Ctrl-D pressed on empty line
		should_exit = true;
		/* XXX send a "goodbye" message so other end doesn't
		 * have to wait for timeout on recv()? */
	} else {
		if (*line) {
			add_history(line);
			mymsg = string(line);
			size_t len = mymsg.length();
			unsigned char iv[16];
			for (size_t i = 0; i < 16; i++) iv[i] = i;
			unsigned char ct[512];
			unsigned char pt[512];
			/* so you can see which bytes were written: */
			memset(ct,0,512);
			memset(pt,0,512);
			
			//unsigned char* session_key = (client_key == NULL) ? server_key : client_key;
			FILE *fptr;
			unsigned char* session_key;
			if (client_key == NULL) {
				fptr = fopen("server_key.bin", "rb");
				if (fptr == NULL) {
					printf("Failed to open file.\n");
				}
				fseek(fptr, 0L, SEEK_END);
				long int file_size = ftell(fptr);
				rewind(fptr);

				session_key = (unsigned char*) malloc(sizeof(unsigned char) * file_size);
				if (session_key == NULL) {
					printf("FAILED TO READ\n");
					fclose(fptr);
				}

				size_t bytes_read = fread(session_key, sizeof(unsigned char), file_size, fptr);
				if (bytes_read != file_size) {
					printf("failed to read\n");
					free(session_key);
					fclose(fptr);
				}

				fclose(fptr);
			}
			else {
				fptr = fopen("client_key.bin", "rb");
				if (fptr == NULL) {
					printf("Failed to open file.\n");
				}
				fseek(fptr, 0L, SEEK_END);
				long int file_size = ftell(fptr);
				rewind(fptr);

				session_key = (unsigned char*) malloc(sizeof(unsigned char) * file_size);
				if (session_key == NULL) {
					printf("FAILED TO READ\n");
					fclose(fptr);
				}

				size_t bytes_read = fread(session_key, sizeof(unsigned char), file_size, fptr);
				if (bytes_read != file_size) {
					printf("failed to read\n");
					free(session_key);
					fclose(fptr);
				}

				fclose(fptr);
			}

			/* encrypt: */
			EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
			if (1!=EVP_EncryptInit_ex(ctx,EVP_aes_256_ctr(),0,session_key,iv))
				ERR_print_errors_fp(stderr);
			int nWritten; /* stores number of written bytes (size of ciphertext) */
			if (1!=EVP_EncryptUpdate(ctx,ct,&nWritten,(unsigned char*)mymsg.c_str(),512))
				ERR_print_errors_fp(stderr);
			EVP_CIPHER_CTX_free(ctx);
			size_t ctlen = nWritten;
			message_length = nWritten;
			// printf("ciphertext of length %i:\n",nWritten);
			// for (size_t i = 0; i < ctlen; i++) {
			// 	printf("%02x",ct[i]);
			// }
			// printf("\n");
				
			//char* hmackey = "asdfasdfasdfasdfasdfasdf";
			unsigned char mac[64]; /* if using sha512 */
			memset(mac,0,64);
			HMAC(EVP_sha512(),session_key,128,ct,512,mac,0);
			//printf("hmac-512(\"%s\"):\n",mymsg);
			for (size_t i = 0; i < 64; i++) {
				//printf("%02x",mac[i]);/
				//IDK DOESN'T WORK WITHOUT LOOP
			}
			//printf("\n");
			
			unsigned char result[576];
			memcpy(result,ct,512);
			memcpy(result+512,mac,64);
			
			//printf("Result(\"%s\"):\n",mymsg);
			//for (size_t i = 0; i < 576; i++) {
			//	printf("%02x",result[i]);
			//}
			//printf("\n");
			
			transcript.push_back("me: " + mymsg);
			ssize_t nbytes;
			if ((nbytes = send(sockfd,result,576,0)) == -1)
				error("send failed");

		}
		pthread_mutex_lock(&qmx);
		mq.push_back({false,mymsg,"me",msg_win});
		pthread_cond_signal(&qcv);
		pthread_mutex_unlock(&qmx);
	}
}

/* if batch is set, don't draw immediately to real screen (use wnoutrefresh
 * instead of wrefresh) */
static void cmd_win_redisplay(bool batch)
{
	int prompt_width = strnlen(rl_display_prompt, 128);
	int cursor_col = prompt_width + strnlen(rl_line_buffer,rl_point);

	werase(cmd_win);
	mvwprintw(cmd_win, 0, 0, "%s%s", rl_display_prompt, rl_line_buffer);
	/* XXX deal with a longer message than the terminal window can show */
	if (cursor_col >= COLS) {
		// Hide the cursor if it lies outside the window. Otherwise it'll
		// appear on the very right.
		curs_set(0);
	} else {
		wmove(cmd_win,0,cursor_col);
		curs_set(1);
	}
	if (batch)
		wnoutrefresh(cmd_win);
	else
		wrefresh(cmd_win);
}

static void readline_redisplay(void)
{
	pthread_mutex_lock(&qmx);
	mq.push_back({false,"","",cmd_win});
	pthread_cond_signal(&qcv);
	pthread_mutex_unlock(&qmx);
}

static void resize(void)
{
	if (LINES >= 3) {
		wresize(msg_win,LINES-2,COLS);
		wresize(sep_win,1,COLS);
		wresize(cmd_win,1,COLS);
		/* now move bottom two to last lines: */
		mvwin(sep_win,LINES-2,0);
		mvwin(cmd_win,LINES-1,0);
	}

	/* Batch refreshes and commit them with doupdate() */
	msg_win_redisplay(true);
	wnoutrefresh(sep_win);
	cmd_win_redisplay(true);
	doupdate();
}

static void init_ncurses(void)
{
	if (!initscr())
		fail_exit("Failed to initialize ncurses");

	if (has_colors()) {
		CHECK(start_color);
		CHECK(use_default_colors);
	}
	CHECK(cbreak);
	CHECK(noecho);
	CHECK(nonl);
	CHECK(intrflush, NULL, FALSE);

	curs_set(1);

	if (LINES >= 3) {
		msg_win = newwin(LINES - 2, COLS, 0, 0);
		sep_win = newwin(1, COLS, LINES - 2, 0);
		cmd_win = newwin(1, COLS, LINES - 1, 0);
	} else {
		// Degenerate case. Give the windows the minimum workable size to
		// prevent errors from e.g. wmove().
		msg_win = newwin(1, COLS, 0, 0);
		sep_win = newwin(1, COLS, 0, 0);
		cmd_win = newwin(1, COLS, 0, 0);
	}
	if (!msg_win || !sep_win || !cmd_win)
		fail_exit("Failed to allocate windows");

	scrollok(msg_win,true);

	if (has_colors()) {
		// Use white-on-blue cells for the separator window...
		CHECK(init_pair, 1, COLOR_WHITE, COLOR_BLUE);
		CHECK(wbkgd, sep_win, COLOR_PAIR(1));
		/* NOTE: -1 is the default background color, which for me does
		 * not appear to be any of the normal colors curses defines. */
		CHECK(init_pair, 2, COLOR_MAGENTA, -1);
	}
	else {
		wbkgd(sep_win,A_STANDOUT); /* c.f. man curs_attr */
	}
	wrefresh(sep_win);
}

static void deinit_ncurses(void)
{
	delwin(msg_win);
	delwin(sep_win);
	delwin(cmd_win);
	endwin();
}

static void init_readline(void)
{
	// Let ncurses do all terminal and signal handling
	rl_catch_signals = 0;
	rl_catch_sigwinch = 0;
	rl_deprep_term_function = NULL;
	rl_prep_term_function = NULL;

	// Prevent readline from setting the LINES and COLUMNS environment
	// variables, which override dynamic size adjustments in ncurses. When
	// using the alternate readline interface (as we do here), LINES and
	// COLUMNS are not updated if the terminal is resized between two calls to
	// rl_callback_read_char() (which is almost always the case).
	rl_change_environment = 0;

	// Handle input by manually feeding characters to readline
	rl_getc_function = readline_getc;
	rl_redisplay_function = readline_redisplay;

	rl_callback_handler_install("> ", msg_typed);
}

static void deinit_readline(void)
{
	rl_callback_handler_remove();
}

static const char* usage =
"Usage: %s [OPTIONS]...\n"
"Secure chat for CSc380.\n\n"
"   -c, --connect HOST  Attempt a connection to HOST.\n"
"   -l, --listen        Listen for new connections.\n"
"   -p, --port    PORT  Listen or connect on PORT (defaults to 1337).\n"
"   -h, --help          show this message and exit.\n";

int main(int argc, char *argv[])
{
	// define long options
	static struct option long_opts[] = {
		{"connect",  required_argument, 0, 'c'},
		{"listen",   no_argument,       0, 'l'},
		{"port",     required_argument, 0, 'p'},
		{"help",     no_argument,       0, 'h'},
		{0,0,0,0}
	};
	// process options:
	char c;
	int opt_index = 0;
	int port = 1337;
	char hostname[HOST_NAME_MAX+1] = "localhost";
	hostname[HOST_NAME_MAX] = 0;
	bool isclient = true;

	while ((c = getopt_long(argc, argv, "c:lp:h", long_opts, &opt_index)) != -1) {
		switch (c) {
			case 'c':
				if (strnlen(optarg,HOST_NAME_MAX))
					strncpy(hostname,optarg,HOST_NAME_MAX);
				break;
			case 'l':
				isclient = false;
				break;
			case 'p':
				port = atoi(optarg);
				break;
			case 'h':
				printf(usage,argv[0]);
				return 0;
			case '?':
				printf(usage,argv[0]);
				return 1;
		}
	}
	if (isclient) {
		initClientNet(hostname,port);
	} else {
		initServerNet(port);
	}

	/* NOTE: these don't work if called from cursesthread */
	init_ncurses();
	init_readline();
	/* start curses thread */
	if (pthread_create(&tcurses,0,cursesthread,0)) {
		fprintf(stderr, "Failed to create curses thread.\n");
	}
	/* start receiver thread: */
	if (pthread_create(&trecv,0,recvMsg,0)) {
		fprintf(stderr, "Failed to create update thread.\n");
	}

	/* put this in the queue to signal need for resize: */
	redraw_data rd = {false,"","",NULL};
	do {
		int c = wgetch(cmd_win);
		switch (c) {
			case KEY_RESIZE:
				pthread_mutex_lock(&qmx);
				mq.push_back(rd);
				pthread_cond_signal(&qcv);
				pthread_mutex_unlock(&qmx);
				break;
				// Ctrl-L -- redraw screen
			// case '\f':
			// 	// Makes the next refresh repaint the screen from scratch
			// 	/* XXX this needs to be done in the curses thread as well. */
			// 	clearok(curscr,true);
			// 	resize();
			// 	break;
			default:
				input = c;
				rl_callback_read_char();
		}
	} while (!should_exit);

	shutdownNetwork();
	deinit_ncurses();
	deinit_readline();
	return 0;
}

/* Let's have one thread responsible for all things curses.  It should
 * 1. Initialize the library
 * 2. Wait for messages (we'll need a mutex-protected queue)
 * 3. Restore terminal / end curses mode? */

/* We'll need yet another thread to listen for incoming messages and
 * post them to the queue. */

void* cursesthread(void* pData)
{
	/* NOTE: these calls only worked from the main thread... */
	// init_ncurses();
	// init_readline();
	while (true) {
		pthread_mutex_lock(&qmx);
		while (mq.empty()) {
			pthread_cond_wait(&qcv,&qmx);
			/* NOTE: pthread_cond_wait will release the mutex and block, then
			 * reaquire it before returning.  Given that only one thread (this
			 * one) consumes elements of the queue, we probably don't have to
			 * check in a loop like this, but in general this is the recommended
			 * way to do it.  See the man page for details. */
		}
		/* at this point, we have control of the queue, which is not empty,
		 * so write all the messages and then let go of the mutex. */
		while (!mq.empty()) {
			redraw_data m = mq.front();
			mq.pop_front();
			if (m.win == cmd_win) {
				cmd_win_redisplay(m.resize);
			} else if (m.resize) {
				resize();
			} else {
				msg_win_redisplay(false,m.msg,m.sender);
				/* Redraw input window to "focus" it (otherwise the cursor
				 * will appear in the transcript which is confusing). */
				cmd_win_redisplay(false);
			}
		}
		pthread_mutex_unlock(&qmx);
	}
	return 0;
}

void* recvMsg(void*)
{
	size_t maxlen = 576;
	char msg[maxlen+1];
	ssize_t nbytes;
	while (1) {
		if ((nbytes = recv(sockfd,msg,maxlen,0)) == -1)
			error("recv failed");
		msg[nbytes] = 0; /* make sure it is null-terminated */
		if (nbytes == 0) {
			/* signal to the main loop that we should quit: */
			should_exit = true;
			return 0;
		}
		
		unsigned char mymsg[512];
		memcpy(mymsg,msg,512);
		unsigned char mymac[64];
		memcpy(mymac,msg+512,64);
		unsigned char pt[512];
		unsigned char iv[16];
		for (size_t i = 0; i < 16; i++) iv[i] = i;
		int nWritten = message_length;
		
		//unsigned char* session_key = (client_key == NULL) ? server_key : client_key;
		FILE *fptr;
		unsigned char* session_key;
		if (client_key == NULL) {
			fptr = fopen("server_key.bin", "rb");
		if (fptr == NULL) {
			printf("Failed to open file.\n");
		}
		fseek(fptr, 0L, SEEK_END);
		long int file_size = ftell(fptr);
		rewind(fptr);

		session_key = (unsigned char*) malloc(sizeof(unsigned char) * file_size);
		if (session_key == NULL) {
			printf("FAILED TO READ\n");
			fclose(fptr);				}

			size_t bytes_read = fread(session_key, sizeof(unsigned char), file_size, fptr);
			if (bytes_read != file_size) {
				printf("failed to read\n");
				free(session_key);
				fclose(fptr);
			}

			fclose(fptr);
		}
		else {
			fptr = fopen("client_key.bin", "rb");
			if (fptr == NULL) {
				printf("Failed to open file.\n");
			}
			fseek(fptr, 0L, SEEK_END);
			long int file_size = ftell(fptr);
			rewind(fptr);

			session_key = (unsigned char*) malloc(sizeof(unsigned char) * file_size);
			if (session_key == NULL) {
				printf("FAILED TO READ\n");
				fclose(fptr);
			}

			size_t bytes_read = fread(session_key, sizeof(unsigned char), file_size, fptr);
			if (bytes_read != file_size) {
				printf("failed to read\n");
				free(session_key);
				fclose(fptr);
			}

			fclose(fptr);
		}
		
		
		unsigned char mac[64]; /* if using sha512 */
		memset(mac,0,64);
		HMAC(EVP_sha512(),session_key,128,(unsigned char*)mymsg,512,mac,0);
		
		int result = memcmp(mac,mymac,64);
		
		if(result == 0){
			/* now decrypt.  NOTE: in counter mode, encryption and decryption are
			 * actually identical, so doing the above again would work.  Also
			 * note that it is crucial to make sure IVs are not reused, though it
			 * Won't be an issue for our hybrid scheme as AES keys are only used
			 * once.  */
			/* wipe out plaintext to be sure it worked: */
			memset(pt,0,512);
			EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
			if (1!=EVP_DecryptInit_ex(ctx,EVP_aes_256_ctr(),0,session_key,iv))
				ERR_print_errors_fp(stderr);
			if (1!=EVP_DecryptUpdate(ctx,pt,&nWritten,mymsg,512))
				ERR_print_errors_fp(stderr);
			// printf("decrypted %i bytes:\n%s\n",nWritten,pt);
			/* NOTE: counter mode will preserve the length (although the person
			 * decrypting needs to know the IV) */
			
			pthread_mutex_lock(&qmx);
			mq.push_back({false,(char*)pt,"Mr Thread",msg_win});
			pthread_cond_signal(&qcv);
			pthread_mutex_unlock(&qmx);
		}
	}
	return 0;
}

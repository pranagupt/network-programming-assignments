/*
Clustershell client operation:
1. Client is run on all machines that are listed in the config file
2. Client connects to server on TCP, and the server IP is specified as a preprocessing directive in the client and server code (SERV_ADDRESS)
3. Client sends command to server, and server coordinates the connected clients to execute the command.
4. Client receives final output to the client which sent the command.
5. There are two processes: one which handles the shell with the client, and one which handles the incoming commands from the server for this client.
6. Each of the two processes has its own TCP connection to the server

Message Design:
Command messages from shell to server: 6 character command header + command
Command messages to from server to client: 6 character command header + 6 digit input header + input + command
Output messages: 6 character header + output 

The first letter of the header is c/o/i for command/output/input
The next 5 characters specify the length of the corresponding transmission

Assumptions:
1. All clients listed in the config file connect in the beginning itself and none of them leave before all commands are over
2. There are no commands that require manual user input from the shell (stdin)
3. Commands, inputs and outputs are of maximum string length 99999 including all ending characters and newlines.
4. Nodes are named as n1, n2, n3, ... nN.
5. Nodes are listed in order in config file and no node number is missing
*/


////////////////////////////////////////////
// Included libraries
////////////////////////////////////////////
#include <stdio.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <errno.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <ctype.h>
#include <signal.h>
#include <pwd.h>
#include <sys/types.h>

////////////////////////////////////////////
// Constants
////////////////////////////////////////////

// server address
#define SERV_ADDRESS "127.0.0.1"
// server port - should be same in client and server code
#define SERV_PORT 12038
// size of header of messages (according to message format)
#define HEADER_SIZE 6
// maximum number of piped commands in a big main command sent by a node for distributed execution
#define MAX_NUMBER_OF_PIPED_COMMANDS 30
// path to config file
#define CONFIG_PATH "config"
// max size of line in config file
#define MAX_SIZE_OF_LINE_IN_CONFIG 30
// the port on which client runs its executioner process - should be same in client and server code
#define CLIEX_PORT 12345
// this goes in the listen() call
#define MAX_CONNECTION_REQUESTS_IN_QUEUE 30


////////////////////////////////////////////
// Data Structures and global variables
////////////////////////////////////////////

int child_pid;

////////////////////////////////////////////
// Functions
////////////////////////////////////////////

/*
    colours implementation
*/
void red () {
  printf("\033[1;31m");
}
void green () {
  printf("\033[1;32m");
}
void cyan () {
  printf("\033[1;36m");
}
void reset () {
  printf("\033[0m");
}


/*
    handling the exiting of one process if the other exits manually.
*/
void sigusr1_handler(int signum){
    exit(1);
}

/*
    execute the given command with the given input on the current node and return the output
*/
char* execute_on_current_node(char* input, char* command){
    char* output = NULL;
    
    // handling cd command
    if (command[0] == 'c' && command[1] == 'd' && command[2] == ' '){
        if (chdir (command + 3) < 0){
            perror("chdir");
            printf("Couldn't change directory.\n");
        }
        output = malloc(sizeof(char));
        strcpy(output, "\0");
        return output;
    }

    // use a pipe to send command input to the process that executes the command as stdin
    int p[2];
    pipe(p);

    write (p[1], input, strlen(input)); // write the input to write end of pipe
    close(p[1]); // don't have to write anything else
    dup2(p[0], 0);  // fd 0 is now the pipe read end

    FILE* cmd_output_f = popen(command, "r"); // Executes command in another process and returns fd for the output
    if (cmd_output_f == NULL){
        perror("popen");
        kill(getppid(), SIGUSR1);
        exit(1);
    }

    // read the output from the fd
    char tmp_buffer[200];
    int curr_size = 1;
    while (fgets(tmp_buffer, sizeof(tmp_buffer), cmd_output_f) != NULL) {
        output = realloc(output, curr_size + strlen(tmp_buffer)); 
        if (output == NULL) {
            printf ("Memory allocation  error. Exiting. \n");
            kill(getppid(), SIGUSR1);
            exit(1);
        } 
        strcpy(output + curr_size - 1, tmp_buffer);
        curr_size += strlen(tmp_buffer); 
    }

    // close
    close(p[0]);
    pclose(cmd_output_f);
    return output;
}

/*
    returns the header string according to message format, given "c" or "o" and the size of the rest of message
*/
char* get_header_str(char* co, int size){
    int num_length = 0;
    int s = size;
    while (s != 0){
        s = s/10;
        num_length++;
    }
    if (size == 0)
        num_length = 1;
    if (num_length > HEADER_SIZE - 1){
        printf ("output is too long, exiting.\n");
        if (getpid() == child_pid)
            kill(getppid(), SIGUSR1);
        else
            kill(child_pid, SIGUSR1);
        exit (1);
    }
    char* zeroes = malloc((HEADER_SIZE - num_length)* sizeof (char));
    for (int i = 0; i < HEADER_SIZE - num_length - 1; i++)
        zeroes[i] = '0';
    zeroes[HEADER_SIZE-num_length - 1] = '\0';
    char* header_str = malloc ((num_length + 1 + 1) * sizeof (char));
    sprintf(header_str, "%s%s%d", co, zeroes, size);
    return header_str;
}

/*
    The parent process calls this function to handle the shell
*/
void shell_handler(int serv_fd){

    while (true) {
        cyan();
        printf("\n[shell]-> ");   // bash style prompt
        reset();

        // read command from stdin
        char *cmd_buff = NULL;
        size_t n = 0;
        ssize_t cmd_inp_len = getline(&cmd_buff, &n, stdin); 
        if (cmd_inp_len <= 0 ||  (cmd_inp_len == 1 && cmd_buff[0] == '\n')) {
            continue;
        }
        cmd_buff[cmd_inp_len-1] = '\0';

        if (strcmp(cmd_buff, "exit") == 0) {
            green();
            printf("\nEXITING SHELL...\n\n");
            reset();
            free(cmd_buff);
            kill(child_pid, SIGUSR1);
            exit(1);
        }

        // send the command to the server
        char* msg = malloc((cmd_inp_len - 1 + HEADER_SIZE + 1) * sizeof(char));
        strcpy(msg, get_header_str ("c", cmd_inp_len-1));
        strcat(msg, cmd_buff);
        int bytes_sent = write (serv_fd, msg, strlen(msg));
        if (bytes_sent < 0){
            perror ("write");
            printf ("Exiting application.\n");
            kill(child_pid, SIGUSR1);
            exit(1);
        }
        if (bytes_sent < strlen(msg)){
            printf ("\nUnable to send the complete command to server. Possible network error. Exiting application.\n");
            kill(child_pid, SIGUSR1);
            exit(1);
        }

        printf ("Command sent to server: %s. Waiting for response....\n", msg);

        // read the output header received as response
        char* output_hdr = malloc((1 + HEADER_SIZE) * sizeof(char));
        int bytes_read = read (serv_fd, output_hdr, HEADER_SIZE);
        if (bytes_read < 0){
            perror ("read");
            kill(child_pid, SIGUSR1);
            exit(1);
        }
        output_hdr[HEADER_SIZE] = '\0';

        // read the rest of the output received as response
        char* output = NULL;
        if (output_hdr[0] == 'o'){
            // setup the receiving char array
            int output_size = atoi(output_hdr+1);
            output = malloc((output_size+1)*sizeof(char));

            // read from socket into output array
            bytes_read = read (serv_fd, output, output_size);
            output[output_size] = '\0';
        }
        else {
            printf ("\ntPossible application or network error detected. Exiting application.\n");
            kill(child_pid, SIGUSR1);
            exit(1);
        }

        // print output to the shell
        green();
        printf("\n%s\n", output);
        reset();
        
        // free heap memory
        free (output_hdr);
        free (output);
    }
}

/*
    The child process calls this function to handle all incoming command requests.
    It creates a new socket, binds it to CLIEX_PORT, and takes commands from the server for execution
*/
void request_handler(){
    

    // bind a socket and start listening on it
    int cliex_sock = socket (PF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (setsockopt(cliex_sock, SOL_SOCKET, SO_REUSEADDR, &(int){ 1 }, sizeof(int)) < 0) {
		perror("setsockopt");
	}

    struct sockaddr_in cliex_addr;
    bzero(&cliex_addr, sizeof(cliex_addr));
    cliex_addr.sin_port = CLIEX_PORT;
    cliex_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    cliex_addr.sin_family = AF_INET;
    bind(cliex_sock, (struct sockaddr*)&cliex_addr, sizeof(cliex_addr));
    listen(cliex_sock, MAX_CONNECTION_REQUESTS_IN_QUEUE);

    // accept connection, execute command, send back output, repeat
    while (true){
        struct sockaddr_in serv_addr;
        int sizereceived = 0;
        int serv_sock = accept(cliex_sock, (struct sockaddr*)&serv_addr,&sizereceived);
        
        int bytes_read = 0;

        // read command header
        char* cmd_hdr = malloc((1 + HEADER_SIZE) * sizeof(char));
        bytes_read = read (serv_sock, cmd_hdr, HEADER_SIZE);
        if (bytes_read < 0){
            perror ("read");
            kill(getppid(), SIGUSR1);
            close (cliex_sock);
            exit(1);
        }
        cmd_hdr[HEADER_SIZE] = '\0';
        // read input header
        char* inp_hdr = malloc((1 + HEADER_SIZE) * sizeof(char));
        bytes_read = read (serv_sock, inp_hdr, HEADER_SIZE);
        if (bytes_read < 0){
            perror ("read");
            kill(getppid(), SIGUSR1);
            close (cliex_sock);
            exit(1);
        }
        inp_hdr[HEADER_SIZE] = '\0';
        // read the input from socket
        char* inp = NULL;
        if (inp_hdr[0] == 'i'){
            // setup the receiving char array
            
            int inp_size = atoi(inp_hdr+1);
            inp = malloc((inp_size+1)*sizeof(char));
            
            // read from socket into input array
            bytes_read = read (serv_sock, inp, inp_size);
            inp[inp_size] = '\0';
        }
        else {
            printf ("\nPossible application or network error detected. Exiting application.\n");
            kill(getppid(), SIGUSR1);
            close (cliex_sock);
            exit(1);
        }
        // read the commad from socket
        char* cmd = NULL;
        if (cmd_hdr[0] == 'c'){
            // setup the receiving char array
            int cmd_size = atoi(cmd_hdr+1);
            cmd = malloc((cmd_size+1)*sizeof(char));
            
            // read from socket into cmd array
            bytes_read = read (serv_sock, cmd, cmd_size);
            cmd[cmd_size] = '\0';
        }
        else {
            printf ("\nPossible application or network error detected. Exiting application.\n");
            kill(getppid(), SIGUSR1);
            close (cliex_sock);
            exit(1);
        }
        // execute command on this machine, with the given input and get the output
        char* output = NULL;
        output = execute_on_current_node(inp, cmd);

        // send output to the server
        char* output_hdr = get_header_str("o", strlen(output));
        char* msg = malloc((HEADER_SIZE + strlen(output) + 1)*sizeof(char));
        strcpy(msg, output_hdr);
        strcat(msg, output);
        int bytes_sent = write(serv_sock, msg, strlen(msg));
        if (bytes_sent < 0){
            perror ("write");
            printf ("Exiting application.\n");
            kill(getppid(), SIGUSR1);
            close (cliex_sock);
            exit(1);
        }
        if (bytes_sent < strlen(msg)){
            printf ("\nUnable to send the complete output to server. Possible network error. Exiting application.\n");
            kill(getppid(), SIGUSR1);
            close (cliex_sock);
            exit(1);
        }
        free (inp);
        free (inp_hdr);
        free (cmd);
        free (cmd_hdr);
        close (serv_sock);

    }
    close (cliex_sock);
    // handle accepted connections
}


/*
    Connects to server and creates the child process to handle incoming commands, while parent handles the shell
*/
int main (int argc, char* argv[]) {

    //change directory to home directory of currently signed in user
    struct passwd *pass = (getpwuid(getuid()));
    char* login_name = pass->pw_name;
    if (login_name == NULL){
        printf("Couldn't access login username. Exiting.\n");
        exit(1);
    }
    printf("Login detected: %s\n", login_name);
    char* home_dir = malloc ((6 + strlen(login_name)) * sizeof(char));
    sprintf(home_dir, "/home/%s", login_name);
    if (chdir (home_dir) < 0){
        perror("chdir");
        printf("Couldn't change directory.\n");
    }

    // register child / parent killer
    signal(SIGUSR1, sigusr1_handler);
    
    // create socket
    int serv_socket = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);

    // prep address structure for connection to server
    struct sockaddr_in serv_addr;
    bzero(&serv_addr, sizeof(serv_addr));
    inet_aton(SERV_ADDRESS, &serv_addr.sin_addr);
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = SERV_PORT;
    
    // connect to server
    if (connect(serv_socket, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) == -1){
        printf("Couldn't connect to server. Exiting application.\n");
        perror("connect");
        exit(1);
    }

    // create a process for listening to commands from server, while parent process handles the shell
    child_pid = fork();
    if (child_pid == 0){ // child process: handles incoming commands from server
        request_handler();
    }
    else if (child_pid > 0){ // parent process: handles shell on client
        shell_handler(serv_socket);
    }
    else{
        printf("Couldn't create a child process. Exiting application.\n");
        exit(1);
    }
    return 0;
}
#include <stdlib.h>
#include <sys/types.h>
#include <pwd.h>
#include <grp.h>
#include <sys/wait.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <src/common/log.h>
#include <src/common/list.h>
#include <src/common/xerrno.h>
#include <src/common/xmalloc.h>
#include <src/common/slurm_protocol_api.h>
#include <src/common/slurm_errno.h>
#include <src/slurmd/task_mgr.h>
#include <src/slurmd/shmem_struct.h>

/*
global variables 
*/

/* file descriptor defines */

#define STDIN_FILENO 0
#define STDOUT_FILENO 1 
#define STDERR_FILENO 2
#define MAX_TASKS_PER_LAUNCH 64
#define CHILD_IN 0
#define CHILD_IN_RD 0
#define CHILD_IN_WR 1
#define CHILD_OUT 2
#define CHILD_OUT_RD 2
#define CHILD_OUT_WR 3
#define CHILD_ERR 4
#define CHILD_ERR_RD 4
#define CHILD_ERR_WR 5

//extern slurmd_shmem_t * shmem_seg ;

/* prototypes */
void slurm_free_task ( void * _task ) ;
int kill_task ( task_t * task ) ;
int interconnect_init ( launch_tasks_msg_t * launch_msg );
int fan_out_task_launch ( launch_tasks_msg_t * launch_msg );
void * task_exec_thread ( void * arg ) ;
int init_parent_pipes ( int * pipes ) ;
void setup_parent_pipes ( int * pipes ) ; 
int setup_child_pipes ( int * pipes ) ;
int forward_io ( task_start_t * task_arg ) ;
void * stdin_io_pipe_thread ( void * arg ) ;
void * stdout_io_pipe_thread ( void * arg ) ;
void * stderr_io_pipe_thread ( void * arg ) ;
int setup_task_env  (task_start_t * task_start ) ;
/******************************************************************
 *task launch method call hierarchy
 *
 *launch_tasks()
 *	interconnect_init()
 *		fan_out_task_launch() (pthread_create)
 *			task_exec_thread() (fork) for task exec
 *			task_exec_thread() (pthread_create) for io piping 
 ******************************************************************/			

/* exported module funtion to launch tasks */
/*launch_tasks should really be named launch_job_step*/
int launch_tasks ( launch_tasks_msg_t * launch_msg )
{
	return interconnect_init ( launch_msg );
}

/* Contains interconnect specific setup instructions and then calls 
 * fan_out_task_launch */
int interconnect_init ( launch_tasks_msg_t * launch_msg )
{
	return fan_out_task_launch ( launch_msg ) ;
}

int fan_out_task_launch ( launch_tasks_msg_t * launch_msg )
{
	int i ;
	int rc ;
	int session_id ;
	
	/* shmem work - see slurmd.c shmem_seg this is probably not needed*/
	slurmd_shmem_t * shmem_ptr = get_shmem ( ) ;
	//slurmd_shmem_t * shmem_ptr = shmem_seg ;

	/*alloc a job_step objec in shmem for this launch_tasks request */
	/*launch_tasks should really be named launch_job_step*/
	job_step_t * curr_job_step = alloc_job_step ( shmem_ptr , launch_msg -> job_id , launch_msg -> job_step_id ) ;
	/* task pointer that will point to shmem task structures as the are allocated*/
	task_t * curr_task = NULL ;
	/* array of pointers used in this function to point to the task_start structure for each task to be
	 * launched*/
	task_start_t * task_start[launch_msg->tasks_to_launch];

	if ( ( session_id = setsid () ) == SLURM_ERROR )
	{
		info ( "set sid failed" );	
	}
	curr_job_step -> session_id = session_id ;

		
	/* launch requested number of threads */
	for ( i = 0 ; i < launch_msg->tasks_to_launch ; i ++ )
	{
		curr_task = alloc_task ( shmem_ptr , curr_job_step );
		task_start[i] = & curr_task -> task_start ;
		
		/* fill in task_start struct */
		task_start[i] -> launch_msg = launch_msg ;
		task_start[i] -> local_task_id = i ; 
		task_start[i] -> inout_dest = *( launch_msg -> streams + ( i * 2 )  ) ; 
		task_start[i] -> err_dest = *( launch_msg -> streams + ( i * 2 ) + 1 ) ; 

		if ( pthread_create ( & task_start[i]->pthread_id , NULL , task_exec_thread , ( void * ) task_start[i] ) )
			goto kill_threads;
	}
	
	/* wait for all the launched threads to finish */
	for ( i = 0 ; i < launch_msg->tasks_to_launch ; i ++ )
	{
		rc = pthread_join( task_start[i]->pthread_id , NULL )  ;
	}
	goto return_label;

	
	kill_threads:
	for (  i-- ; i >= 0  ; i -- )
	{
		rc = pthread_kill ( task_start[i]->pthread_id , SIGKILL ) ;
	}

	return_label:
		rel_shmem ( shmem_ptr ) ;
	return SLURM_SUCCESS ;
}

int forward_io ( task_start_t * task_arg ) 
{
	pthread_attr_t pthread_attr ;
	struct sigaction newaction ;
        struct sigaction oldaction ;
	int local_errno;
	newaction . sa_handler = SIG_IGN ;
#define STDIN_OUT_SOCK 0
#define SIG_STDERR_SOCK 1 
	/* open stdout & stderr sockets */
	if ( ( task_arg->sockets[STDIN_OUT_SOCK] = slurm_open_stream ( & ( task_arg -> inout_dest ) ) ) == SLURM_PROTOCOL_ERROR )
	{
		local_errno = errno ;	
		info ( "error opening socket to srun to pipe stdout errno %i" , local_errno ) ;
		pthread_exit ( 0 ) ;
	}
	
	if ( ( task_arg->sockets[SIG_STDERR_SOCK] = slurm_open_stream ( &( task_arg -> err_dest ) ) ) == SLURM_PROTOCOL_ERROR )
	{
		local_errno = errno ;	
		info ( "error opening socket to srun to pipe stdout errno %i" , local_errno ) ;
		pthread_exit ( 0 ) ;
	}
	
	sigaction(SIGPIPE, &newaction, &oldaction); /* ignore tty input */
	
	/* spawn io pipe threads */
	pthread_attr_init( & pthread_attr ) ;
	//pthread_attr_setdetachstate ( & pthread_attr , PTHREAD_CREATE_DETACHED ) ;
	if ( pthread_create ( & task_arg->io_pthread_id[STDIN_FILENO] , NULL , stdin_io_pipe_thread , task_arg ) )
		goto return_label;
	if ( pthread_create ( & task_arg->io_pthread_id[STDOUT_FILENO] , NULL , stdout_io_pipe_thread , task_arg ) )
		goto kill_stdin_thread;
	if ( pthread_create ( & task_arg->io_pthread_id[STDERR_FILENO] , NULL , stderr_io_pipe_thread , task_arg ) )
		goto kill_stdout_thread;

	/* threads have been detatched*/
	pthread_join ( task_arg->io_pthread_id[STDERR_FILENO] , NULL ) ;
	info ( "errexit" ) ;
	pthread_join ( task_arg->io_pthread_id[STDOUT_FILENO] , NULL ) ;
	info ( "outexit" ) ;
	pthread_kill (  task_arg->io_pthread_id[STDIN_FILENO] , SIGKILL );
	
	goto return_label;

	kill_stdout_thread:
		pthread_kill ( task_arg->io_pthread_id[STDOUT_FILENO] , SIGKILL );
	kill_stdin_thread:
		pthread_kill ( task_arg->io_pthread_id[STDIN_FILENO] , SIGKILL );
	return_label:
	return SLURM_SUCCESS ;
}

void * stdin_io_pipe_thread ( void * arg )
{
	task_start_t * io_arg = ( task_start_t * ) arg ;
	char buffer[SLURMD_IO_MAX_BUFFER_SIZE] ;
	int bytes_read ;
	int bytes_written ;
	struct sigaction newaction ;
        struct sigaction oldaction ;
	newaction . sa_handler = SIG_IGN ;
	sigaction(SIGPIPE, &newaction, &oldaction); /* ignore tty input */
	
	while ( true )
	{
		if ( ( bytes_read = slurm_read_stream ( io_arg->sockets[0] , buffer , SLURMD_IO_MAX_BUFFER_SIZE ) ) == SLURM_PROTOCOL_ERROR )
		{
			info ( "error reading stdin stream for task %i", 1 ) ;
			pthread_exit ( NULL ) ; 
		}
		write ( 1 ,  "stdin-", 6 ) ;
		write ( 1 ,  buffer , bytes_read ) ;
		if ( ( bytes_written = write ( io_arg->pipes[CHILD_IN_WR] , buffer , bytes_read ) ) <= 0 )
		{
			info ( "error sending stdin stream for task %i", 1 ) ;
			pthread_exit ( NULL ) ;
		}
	}
}

void * stdout_io_pipe_thread ( void * arg )
{
	task_start_t * io_arg = ( task_start_t * ) arg ;
	char buffer[SLURMD_IO_MAX_BUFFER_SIZE] ;
	int bytes_read ;
	int sock_bytes_written ;
	int local_errno ;
	struct sigaction newaction ;
        struct sigaction oldaction ;
	newaction . sa_handler = SIG_IGN ;
	sigaction(SIGPIPE, &newaction, &oldaction); /* ignore tty input */
	
	while ( true )
	{
		if ( ( bytes_read = read ( io_arg->pipes[CHILD_OUT_RD] , buffer , SLURMD_IO_MAX_BUFFER_SIZE ) ) <= 0 )
		{
			local_errno = errno ;	
			info ( "error reading stdout stream for task %i, errno %i , bytes read %i ", 1 , local_errno , bytes_read ) ;

			pthread_exit ( NULL ) ;
		}
		write ( 1 ,  buffer , bytes_read ) ;
		if ( ( sock_bytes_written = slurm_write_stream ( io_arg->sockets[0] , buffer , bytes_read ) ) == SLURM_PROTOCOL_ERROR )
		{
			local_errno = errno ;	
			info ( "error sending stdout stream for task %i , errno %i", 1 , local_errno ) ;
			pthread_exit ( NULL ) ; 
		}
	}
}

void * stderr_io_pipe_thread ( void * arg )
{
	task_start_t * io_arg = ( task_start_t * ) arg ;
	char buffer[SLURMD_IO_MAX_BUFFER_SIZE] ;
	int bytes_read ;
	int sock_bytes_written ;
	int local_errno ;

	while ( true )
	{
		if ( ( bytes_read = read ( io_arg->pipes[CHILD_ERR_RD] , buffer , SLURMD_IO_MAX_BUFFER_SIZE ) ) <= 0 )
		{
			local_errno = errno ;
			info ( "error reading stderr stream for task %i, errno %i , bytes read %i ", 1 , local_errno , bytes_read ) ;
			pthread_exit ( NULL ) ;
		}
		write ( 2 ,  buffer , bytes_read ) ;
		if ( ( sock_bytes_written = slurm_write_stream ( io_arg->sockets[1] , buffer , bytes_read ) ) == SLURM_PROTOCOL_ERROR )
		{
			info ( "error sending stderr stream for task %i", 1 ) ;
			pthread_exit ( NULL ) ; 
		}
	}
}


void * task_exec_thread ( void * arg )
{
	task_start_t * task_start = ( task_start_t * ) arg ;
	launch_tasks_msg_t * launch_msg = task_start -> launch_msg ;
	int * pipes = task_start->pipes ;
	int rc ;
	int cpid ;
	struct passwd * pwd ;
	struct sigaction newaction ;
        struct sigaction oldaction ;

	newaction . sa_handler = SIG_IGN ;
		 
	/* create pipes to read child stdin, stdout, sterr */
	init_parent_pipes ( task_start->pipes ) ;

#define FORK_ERROR -1
#define CHILD_PROCCESS 0
	switch ( ( cpid = fork ( ) ) )
	{
		case FORK_ERROR:
			break ;

		case CHILD_PROCCESS:
			info ("CLIENT PROCESS");
			sigaction(SIGTTOU, &newaction, &oldaction); /* ignore tty output */
			sigaction(SIGTTIN, &newaction, &oldaction); /* ignore tty input */
			sigaction(SIGTSTP, &newaction, &oldaction); /* ignore user */
			
			/* setup std stream pipes */
			setup_child_pipes ( pipes ) ;

			/* get passwd file info */
			if ( ( pwd = getpwuid ( launch_msg->uid ) ) == NULL )
			{
				info ( "user id not found in passwd file" ) ;
				_exit ( SLURM_FAILURE ) ;
			}
			
			/* setuid and gid*/
			if ( ( rc = setuid ( launch_msg->uid ) ) == SLURM_ERROR ) 
			{
				info ( "set user id failed " ) ;
				_exit ( SLURM_FAILURE ) ;
			}
			
			if ( ( rc = setgid ( pwd -> pw_gid ) ) == SLURM_ERROR )
			{
				info ( "set group id failed " ) ;
				_exit ( SLURM_FAILURE ) ;
			}
			/* initgroups */
			/*if ( ( rc = initgroups ( pwd ->pw_name , pwd -> pw_gid ) ) == SLURM_ERROR )
			{
				info ( "init groups failed " ) ;
				_exit ( SLURM_FAILURE ) ;
			}
			*/

			/* run bash and cmdline */
			debug( "cwd %s", launch_msg->cwd ) ;
			chdir ( launch_msg->cwd ) ;
			debug( "cmdline %s", launch_msg->cmd_line ) ;
			execl ("/bin/bash", "bash", "-c", launch_msg->cmd_line, 0);
	
			//execle ( "/bin/sh", launch_msg->cmd_line , launch_msg->env );
			close ( STDIN_FILENO );
			close ( STDOUT_FILENO );
			close ( STDERR_FILENO );
			_exit ( SLURM_SUCCESS ) ;
			
		default: /*parent proccess */
			task_start->exec_pid = cpid ;
			setup_parent_pipes ( task_start->pipes ) ;
			forward_io ( arg ) ;
			waitpid ( cpid , NULL , 0 ) ;
	}
	return ( void * ) SLURM_SUCCESS ;
}

void setup_parent_pipes ( int * pipes )
{
	close ( pipes[CHILD_IN_RD] ) ;
	close ( pipes[CHILD_OUT_WR] ) ;
	close ( pipes[CHILD_ERR_WR] ) ;
}

int init_parent_pipes ( int * pipes )
{
	int rc ;
	/* open pipes to be used in dup after fork */
	if( ( rc = pipe ( & pipes[CHILD_IN] ) ) ) 
	{
		return ESLRUMD_PIPE_ERROR_ON_TASK_SPAWN ;
	}
	if( ( rc = pipe ( & pipes[CHILD_OUT] ) ) ) 
	{
		return ESLRUMD_PIPE_ERROR_ON_TASK_SPAWN ;
	}
	if( ( rc = pipe ( & pipes[CHILD_ERR] ) ) ) 
	{
		return ESLRUMD_PIPE_ERROR_ON_TASK_SPAWN ;
	}
	return SLURM_SUCCESS ;
}

int setup_child_pipes ( int * pipes )
{
	int error_code = 0 ;
	int local_errno;

	/*dup stdin*/
	//close ( STDIN_FILENO );
	if ( SLURM_ERROR == ( error_code |= dup2 ( pipes[CHILD_IN_RD] , STDIN_FILENO ) ) ) 
	{
		local_errno = errno ;
		info ("dup failed on child standard in pipe, errno %i" , local_errno );
		//return error_code ;
	}
	close ( CHILD_IN_RD );
	close ( CHILD_IN_WR );

	/*dup stdout*/
	//close ( STDOUT_FILENO );
	if ( SLURM_ERROR == ( error_code |= dup2 ( pipes[CHILD_OUT_WR] , STDOUT_FILENO ) ) ) 
	{
		local_errno = errno ;
		info ("dup failed on child standard out pipe, errno %i" , local_errno );
		//return error_code ;
	}
	close ( CHILD_OUT_RD );
	close ( CHILD_OUT_WR );

	/*dup stderr*/
	//close ( STDERR_FILENO );
	if ( SLURM_ERROR == ( error_code |= dup2 ( pipes[CHILD_ERR_WR] , STDERR_FILENO ) ) ) 
	{
		local_errno = errno ;
		info ("dup failed on child standard err pipe, errno %i" , local_errno );
		//return error_code ;
	}
	close ( CHILD_ERR_RD );
	close ( CHILD_ERR_WR );

	return error_code ;
}

int kill_tasks ( kill_tasks_msg_t * kill_task_msg )
{
	int error_code = SLURM_SUCCESS ;

	return error_code ;
}


int kill_task ( task_t * task )
{
	return SLURM_SUCCESS ;
}

int setup_task_env  (task_start_t * task_start )
{
	int i ;
	for ( i = 0 ; i < task_start -> launch_msg -> envc ; i ++ )
	{
		char * env_var = xmalloc ( strlen (  task_start -> launch_msg -> env[i] ) ) ;
		memcpy ( env_var , task_start -> launch_msg -> env[i] , strlen (  task_start -> launch_msg -> env[i] ) ) ;
		putenv ( env_var ) ;
	}
	return SLURM_SUCCESS ;
}

#include <mach/exception.h>
#include <mach/mach.h>
#include <mach/task.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>

void
generate_native_abort (void)
{
	*(char volatile *)NULL = 1;
}

static int
handle_exception (char *addr, void *ip, integer_t code);

extern boolean_t
exc_server (mach_msg_header_t *, mach_msg_header_t *);

kern_return_t
exception_raise (
	mach_port_t,
	mach_port_t,
	mach_port_t,
	exception_type_t,
	exception_data_t,
	mach_msg_type_number_t
);

kern_return_t
exception_raise_state (
	mach_port_t,
	mach_port_t,
	mach_port_t,
	exception_type_t,
	exception_data_t,
	mach_msg_type_number_t,
	thread_state_flavor_t *,
	thread_state_t,
	mach_msg_type_number_t,
	thread_state_t,
	mach_msg_type_number_t *
);

kern_return_t
exception_raise_state_identity (
	mach_port_t,
	mach_port_t,
	mach_port_t,
	exception_type_t,
	exception_data_t,
	mach_msg_type_number_t,
	thread_state_flavor_t *,
	thread_state_t,
	mach_msg_type_number_t,
	thread_state_t,
	mach_msg_type_number_t *
);

#define MAX_EXCEPTION_PORTS 16

static struct
{
	mach_msg_type_number_t count;
	exception_mask_t       masks     [MAX_EXCEPTION_PORTS];
	exception_handler_t    ports     [MAX_EXCEPTION_PORTS];
	exception_behavior_t   behaviors [MAX_EXCEPTION_PORTS];
	thread_state_flavor_t  flavors   [MAX_EXCEPTION_PORTS];
} old_exception_ports;

static mach_port_t exception_port;

static void *
handler_thread (void *unused)
{
	mach_msg_return_t status;

	struct {
		mach_msg_header_t head;
		char data [256];
	} reply;

	struct {
		mach_msg_header_t head;
		mach_msg_body_t msgh_body;
		char data [1024];
	} message;

/* MONO */
	void *handle = dlopen (NULL, RTLD_LAZY | RTLD_LOCAL);
	void (*mono_threads_attach_tools_thread) (void)
		= dlsym (handle, "mono_threads_attach_tools_thread");
	mono_threads_attach_tools_thread ();
	dlclose (handle);
/* END MONO */

	for (;;) {

		status = mach_msg (
			&message.head,
			MACH_RCV_MSG | MACH_RCV_LARGE,
			0,
			sizeof (message),
			exception_port,
			MACH_MSG_TIMEOUT_NONE,
			MACH_PORT_NULL
		);

		exc_server (&message.head, &reply.head);

		status = mach_msg (
			&reply.head,
			MACH_SEND_MSG,
			reply.head.msgh_size,
			0,
			MACH_PORT_NULL,
			MACH_MSG_TIMEOUT_NONE,
			MACH_PORT_NULL
		);

	}

}

static kern_return_t
forward_exception (
	mach_port_t thread,
	mach_port_t task,
	exception_type_t exception,
	exception_data_t data,
	mach_msg_type_number_t data_count
)
{
	int                     i;
	kern_return_t           status;
	mach_port_t             port;
	exception_behavior_t    behavior;
	thread_state_flavor_t   flavor;
	thread_state_data_t     thread_state;
	mach_msg_type_number_t  thread_state_count = THREAD_STATE_MAX;

	for (i = 0; i < old_exception_ports.count; ++i)
		if (old_exception_ports.masks [i] & (1 << exception))
			break;

	if (i == old_exception_ports.count)
		return KERN_FAILURE;

	port = old_exception_ports.ports [i];
	behavior = old_exception_ports.behaviors [i];
	flavor = old_exception_ports.flavors [i];

	if (behavior != EXCEPTION_DEFAULT) {
		status = thread_get_state (
			thread,
			flavor,
			thread_state,
			&thread_state_count
		);
		if (status != KERN_SUCCESS)
			return KERN_FAILURE;
	}

	switch (behavior) {
		case EXCEPTION_DEFAULT:
			status = exception_raise (
				port,
				thread,
				task,
				exception,
				data,
				data_count
			);
			break;

		case EXCEPTION_STATE:
			status = exception_raise_state (
				port,
				thread,
				task,
				exception,
				data,
				data_count,
				&flavor,
				thread_state,
				thread_state_count,
				thread_state,
				&thread_state_count
			);
			break;

		case EXCEPTION_STATE_IDENTITY:
			status = exception_raise_state_identity (
				port,
				thread,
				task,
				exception,
				data,
				data_count,
				&flavor,
				thread_state,
				thread_state_count,
				thread_state,
				&thread_state_count
			);
			break;

		default:
			status = KERN_FAILURE;
			break;
	}

	if (behavior == EXCEPTION_STATE || behavior == EXCEPTION_STATE_IDENTITY) {
		status = thread_set_state (
			thread,
			flavor,
			thread_state,
			thread_state_count
		);
		if (status != KERN_SUCCESS)
			return KERN_FAILURE;
	}

	return status;
}

kern_return_t
catch_exception_raise (
	mach_port_t             exception_port,
	mach_port_t             thread,
	mach_port_t             task,
	exception_type_t        exception,
	exception_data_t        code,
	mach_msg_type_number_t  code_count
)
{
	kern_return_t            status;
	x86_exception_state32_t  exception_state;
	mach_msg_type_number_t   exception_state_count = x86_EXCEPTION_STATE32_COUNT;
	char                    *fault_address;
	x86_thread_state32_t     thread_state;
	mach_msg_type_number_t   thread_state_count = x86_THREAD_STATE32_COUNT;
	void                    *fault_ip;

	if (exception != EXC_BAD_ACCESS)
		return forward_exception (thread, task, exception, code, code_count);

	status = thread_get_state (
		thread,
		x86_EXCEPTION_STATE32,
		(natural_t *)&exception_state,
		&exception_state_count
	);

	if (status != KERN_SUCCESS)
		return KERN_FAILURE;

	fault_address = (char *)exception_state.__faultvaddr;

	status = thread_get_state (
		thread,
		x86_THREAD_STATE32,
		(natural_t *)&thread_state,
		&thread_state_count
	);

	fault_ip = (void *)thread_state.__eip;

	if (handle_exception (fault_address, fault_ip, code [0]))
		return KERN_SUCCESS;

	return forward_exception (thread, task, exception, code, code_count);
}

kern_return_t
catch_exception_raise_state (
	mach_port_name_t        exception_port,
	int                     exception,
	exception_data_t        code,
	mach_msg_type_number_t  code_count,
	int                     flavor,
	thread_state_t          old_state,
	int                     old_state_count,
	thread_state_t          new_state,
	int                     new_state_count
)
{
	return KERN_INVALID_ARGUMENT;
}

kern_return_t
catch_exception_raise_state_identity (
	mach_port_name_t        exception_port,
	mach_port_t             thread,
	mach_port_t             task,
	int                     exception,
	exception_data_t        code,
	mach_msg_type_number_t  code_count,
	int                     flavor,
	thread_state_t          old_state,
	int                     old_state_count,
	thread_state_t          new_state,
	int                     new_state_count
)
{
	return KERN_INVALID_ARGUMENT;
}

typedef struct find_jit_info_closure
{
	void *(*jit_info_table_find) (void *domain, char *ip);
	void **jit_info;
	void *ip;
} find_jit_info_closure;

static void
find_jit_info (void *domain, void *user_data)
{
	find_jit_info_closure *closure = (find_jit_info_closure *)user_data;
	if (!*closure->jit_info)
		*closure->jit_info = closure->jit_info_table_find (domain, closure->ip);
}

static int
handle_exception (
	char *address,
	void *ip,
	integer_t code
)
{
/* MONO */
	void *handle = dlopen (NULL, RTLD_LAZY | RTLD_LOCAL);
	void *(*mono_jit_info_table_find)(void *, char *)
		= dlsym (handle, "mono_jit_info_table_find");
	void *(*mono_domain_foreach) (void *, void *)
		= dlsym (handle, "mono_domain_foreach");
	void *jit_info = NULL;
	find_jit_info_closure closure = { mono_jit_info_table_find, &jit_info, ip };
	mono_domain_foreach (find_jit_info, &closure);
	dlclose (handle);

	if (jit_info) {
		fprintf (stderr, "Got managed exception; forwarding.\n");
		return 0;
	}

	fprintf (stderr, "Got unmanaged exception; generating crash report.\n");
	exit (1);

	fprintf (stderr, "Some other circumstance; resuming.\n");
	return 1;
/* END MONO */
}

void
install_mach_exception_handlers (void)
{
	kern_return_t status;
	mach_port_t self;
	pthread_t thread;
	pthread_attr_t attr;
	exception_mask_t mask;

	self = mach_task_self ();

	status = mach_port_allocate (
		self,
		MACH_PORT_RIGHT_RECEIVE,
		&exception_port
	);

	status = mach_port_insert_right (
		self,
		exception_port,
		exception_port,
		MACH_MSG_TYPE_MAKE_SEND
	);

	mask = EXC_MASK_BAD_ACCESS;

	status = task_get_exception_ports (
		self,
		mask,
		old_exception_ports.masks,
		&old_exception_ports.count,
		old_exception_ports.ports,
		old_exception_ports.behaviors,
		old_exception_ports.flavors
	);

	status = task_set_exception_ports (
		self,
		mask,
		exception_port,
		EXCEPTION_DEFAULT,
		i386_THREAD_STATE
	);

	pthread_attr_init (&attr);
	pthread_attr_setdetachstate (&attr, PTHREAD_CREATE_DETACHED);
	pthread_create (&thread, &attr, handler_thread, NULL);
	pthread_attr_destroy (&attr);
}

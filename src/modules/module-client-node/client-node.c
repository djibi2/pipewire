/* PipeWire
 * Copyright (C) 2015 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <string.h>
#include <stddef.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/eventfd.h>

#include <spa/node/node.h>
#include <spa/lib/pod.h>

#include "pipewire/pipewire.h"
#include "pipewire/interfaces.h"
#include "pipewire/private.h"

#include "pipewire/core.h"
#include "modules/spa/spa-node.h"
#include "client-node.h"
#include "transport.h"

/** \cond */

#define MAX_INPUTS       64
#define MAX_OUTPUTS      64

#define MAX_BUFFERS      64

#define CHECK_IN_PORT_ID(this,d,p)       ((d) == SPA_DIRECTION_INPUT && (p) < MAX_INPUTS)
#define CHECK_OUT_PORT_ID(this,d,p)      ((d) == SPA_DIRECTION_OUTPUT && (p) < MAX_OUTPUTS)
#define CHECK_PORT_ID(this,d,p)          (CHECK_IN_PORT_ID(this,d,p) || CHECK_OUT_PORT_ID(this,d,p))
#define CHECK_FREE_IN_PORT(this,d,p)     (CHECK_IN_PORT_ID(this,d,p) && !(this)->in_ports[p].valid)
#define CHECK_FREE_OUT_PORT(this,d,p)    (CHECK_OUT_PORT_ID(this,d,p) && !(this)->out_ports[p].valid)
#define CHECK_FREE_PORT(this,d,p)        (CHECK_FREE_IN_PORT (this,d,p) || CHECK_FREE_OUT_PORT (this,d,p))
#define CHECK_IN_PORT(this,d,p)          (CHECK_IN_PORT_ID(this,d,p) && (this)->in_ports[p].valid)
#define CHECK_OUT_PORT(this,d,p)         (CHECK_OUT_PORT_ID(this,d,p) && (this)->out_ports[p].valid)
#define CHECK_PORT(this,d,p)             (CHECK_IN_PORT (this,d,p) || CHECK_OUT_PORT (this,d,p))

#define GET_IN_PORT(this,p)	(&this->in_ports[p])
#define GET_OUT_PORT(this,p)	(&this->out_ports[p])
#define GET_PORT(this,d,p)	(d == SPA_DIRECTION_INPUT ? GET_IN_PORT(this,p) : GET_OUT_PORT(this,p))

#define CHECK_PORT_BUFFER(this,b,p)      (b < p->n_buffers)

struct proxy_buffer {
	struct spa_buffer *outbuf;
	struct spa_buffer buffer;
	struct spa_meta metas[4];
	struct spa_data datas[4];
	off_t offset;
	size_t size;
	bool outstanding;
};

struct proxy_port {
	bool valid;
	struct spa_port_info info;

	bool have_format;
	uint32_t n_params;
	struct spa_pod **params;
	struct spa_port_io *io;

	uint32_t n_buffers;
	struct proxy_buffer buffers[MAX_BUFFERS];
};

struct proxy {
	struct spa_node node;

	struct impl *impl;

	struct spa_type_map *map;
	struct spa_log *log;
	struct spa_loop *data_loop;

	const struct spa_node_callbacks *callbacks;
	void *callbacks_data;

	struct pw_resource *resource;

	struct spa_source data_source;
	int writefd;

	uint32_t max_inputs;
	uint32_t n_inputs;
	uint32_t max_outputs;
	uint32_t n_outputs;
	struct proxy_port in_ports[MAX_INPUTS];
	struct proxy_port out_ports[MAX_OUTPUTS];

	uint32_t n_params;
	struct spa_pod **params;

	uint8_t format_buffer[1024];
	uint32_t seq;
};

struct impl {
	struct pw_client_node this;

	bool client_reuse;

	struct pw_core *core;
	struct pw_type *t;

	struct proxy proxy;

	struct pw_client_node_transport *transport;

	struct spa_hook node_listener;
	struct spa_hook resource_listener;

	int fds[2];
	int other_fds[2];

	uint32_t input_ready;
	bool out_pending;
};

/** \endcond */

static int clear_buffers(struct proxy *this, struct proxy_port *port)
{
	if (port->n_buffers) {
		spa_log_info(this->log, "proxy %p: clear buffers", this);
		port->n_buffers = 0;
	}
	return 0;
}

static int spa_proxy_node_enum_params(struct spa_node *node,
				      uint32_t id, uint32_t *index,
				      const struct spa_pod *filter,
				      struct spa_pod **result,
				      struct spa_pod_builder *builder)
{
	struct proxy *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);
	spa_return_val_if_fail(builder != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct proxy, node);

	while (true) {
		struct spa_pod *param;

		if (*index >= this->n_params)
			return 0;

		param = this->params[(*index)++];

		if (!spa_pod_is_object_id(param, id))
			continue;

		if (spa_pod_filter(builder, result, param, filter) == 0)
			break;
	}
	return 1;
}

static int spa_proxy_node_set_param(struct spa_node *node, uint32_t id, uint32_t flags,
				    const struct spa_pod *param)
{
	struct proxy *this;

	if (node == NULL)
		return -EINVAL;

	this = SPA_CONTAINER_OF(node, struct proxy, node);

	if (this->resource == NULL)
		return 0;

	pw_client_node_resource_set_param(this->resource, this->seq, id, flags, param);

	return SPA_RESULT_RETURN_ASYNC(this->seq++);
}

static inline void do_flush(struct proxy *this)
{
	uint64_t cmd = 1;
	if (write(this->writefd, &cmd, 8) != 8)
		spa_log_warn(this->log, "proxy %p: error flushing : %s", this, strerror(errno));

}

static int spa_proxy_node_send_command(struct spa_node *node, const struct spa_command *command)
{
	struct proxy *this;
	int res = 0;
	struct pw_type *t;

	if (node == NULL || command == NULL)
		return -EINVAL;

	this = SPA_CONTAINER_OF(node, struct proxy, node);

	if (this->resource == NULL)
		return 0;

	t = this->impl->t;

	if (SPA_COMMAND_TYPE(command) == t->command_node.ClockUpdate) {
		pw_client_node_resource_command(this->resource, this->seq++, command);
	} else {
		/* send start */
		pw_client_node_resource_command(this->resource, this->seq, command);
		res = SPA_RESULT_RETURN_ASYNC(this->seq++);
	}
	return res;
}

static int
spa_proxy_node_set_callbacks(struct spa_node *node,
			     const struct spa_node_callbacks *callbacks,
			     void *data)
{
	struct proxy *this;

	if (node == NULL)
		return -EINVAL;

	this = SPA_CONTAINER_OF(node, struct proxy, node);
	this->callbacks = callbacks;
	this->callbacks_data = data;

	return 0;
}

static int
spa_proxy_node_get_n_ports(struct spa_node *node,
			   uint32_t *n_input_ports,
			   uint32_t *max_input_ports,
			   uint32_t *n_output_ports,
			   uint32_t *max_output_ports)
{
	struct proxy *this;

	if (node == NULL)
		return -EINVAL;

	this = SPA_CONTAINER_OF(node, struct proxy, node);

	if (n_input_ports)
		*n_input_ports = this->n_inputs;
	if (max_input_ports)
		*max_input_ports = this->max_inputs == 0 ? this->n_inputs : this->max_inputs;
	if (n_output_ports)
		*n_output_ports = this->n_outputs;
	if (max_output_ports)
		*max_output_ports = this->max_outputs == 0 ? this->n_outputs : this->max_outputs;

	return 0;
}

static int
spa_proxy_node_get_port_ids(struct spa_node *node,
			    uint32_t n_input_ports,
			    uint32_t *input_ids,
			    uint32_t n_output_ports,
			    uint32_t *output_ids)
{
	struct proxy *this;
	int c, i;

	if (node == NULL)
		return -EINVAL;

	this = SPA_CONTAINER_OF(node, struct proxy, node);

	if (input_ids) {
		for (c = 0, i = 0; i < MAX_INPUTS && c < n_input_ports; i++) {
			if (this->in_ports[i].valid)
				input_ids[c++] = i;
		}
	}
	if (output_ids) {
		for (c = 0, i = 0; i < MAX_OUTPUTS && c < n_output_ports; i++) {
			if (this->out_ports[i].valid)
				output_ids[c++] = i;
		}
	}
	return 0;
}

static void
do_update_port(struct proxy *this,
	       enum spa_direction direction,
	       uint32_t port_id,
	       uint32_t change_mask,
	       uint32_t n_params,
	       const struct spa_pod **params,
	       const struct spa_port_info *info)
{
	struct proxy_port *port;
	struct pw_type *t = this->impl->t;

	port = GET_PORT(this, direction, port_id);

	if (change_mask & PW_CLIENT_NODE_PORT_UPDATE_PARAMS) {
		int i;

		port->have_format = false;

		spa_log_info(this->log, "proxy %p: update %d params", this, n_params);
		for (i = 0; i < port->n_params; i++)
			free(port->params[i]);
		port->n_params = n_params;
		port->params = realloc(port->params, port->n_params * sizeof(struct spa_pod *));

		for (i = 0; i < port->n_params; i++) {
			port->params[i] = pw_spa_pod_copy(params[i]);
			if (spa_pod_is_object_id(port->params[i], t->param.idFormat))
				port->have_format = true;
		}

	}

	if (change_mask & PW_CLIENT_NODE_PORT_UPDATE_INFO && info)
		port->info = *info;

	if (!port->valid) {
		spa_log_info(this->log, "proxy %p: adding port %d", this, port_id);
		port->have_format = false;
		port->valid = true;

		if (direction == SPA_DIRECTION_INPUT)
			this->n_inputs++;
		else
			this->n_outputs++;
	}
}

static void
clear_port(struct proxy *this,
	   struct proxy_port *port, enum spa_direction direction, uint32_t port_id)
{
	do_update_port(this,
		       direction,
		       port_id,
		       PW_CLIENT_NODE_PORT_UPDATE_PARAMS |
		       PW_CLIENT_NODE_PORT_UPDATE_INFO, 0, NULL, NULL);
	clear_buffers(this, port);
}

static void do_uninit_port(struct proxy *this, enum spa_direction direction, uint32_t port_id)
{
	struct proxy_port *port;

	spa_log_info(this->log, "proxy %p: removing port %d", this, port_id);

	if (direction == SPA_DIRECTION_INPUT) {
		port = GET_IN_PORT(this, port_id);
		this->n_inputs--;
	} else {
		port = GET_OUT_PORT(this, port_id);
		this->n_outputs--;
	}
	clear_port(this, port, direction, port_id);
	port->valid = false;
}

static int
spa_proxy_node_add_port(struct spa_node *node, enum spa_direction direction, uint32_t port_id)
{
	struct proxy *this;
	struct proxy_port *port;

	if (node == NULL)
		return -EINVAL;

	this = SPA_CONTAINER_OF(node, struct proxy, node);

	if (!CHECK_FREE_PORT(this, direction, port_id))
		return -EINVAL;

	port = GET_PORT(this, direction, port_id);
	clear_port(this, port, direction, port_id);

	return 0;
}

static int
spa_proxy_node_remove_port(struct spa_node *node, enum spa_direction direction, uint32_t port_id)
{
	struct proxy *this;

	if (node == NULL)
		return -EINVAL;

	this = SPA_CONTAINER_OF(node, struct proxy, node);

	if (!CHECK_PORT(this, direction, port_id))
		return -EINVAL;

	do_uninit_port(this, direction, port_id);

	return 0;
}

static int
spa_proxy_node_port_get_info(struct spa_node *node,
			     enum spa_direction direction,
			     uint32_t port_id, const struct spa_port_info **info)
{
	struct proxy *this;
	struct proxy_port *port;

	if (node == NULL || info == NULL)
		return -EINVAL;

	this = SPA_CONTAINER_OF(node, struct proxy, node);

	if (!CHECK_PORT(this, direction, port_id))
		return -EINVAL;

	port = GET_PORT(this, direction, port_id);
	*info = &port->info;

	return 0;
}

static int
spa_proxy_node_port_enum_params(struct spa_node *node,
				enum spa_direction direction, uint32_t port_id,
				uint32_t id, uint32_t *index,
				const struct spa_pod *filter,
				struct spa_pod **result,
				struct spa_pod_builder *builder)
{
	struct proxy *this;
	struct proxy_port *port;

	spa_return_val_if_fail(node != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);
	spa_return_val_if_fail(builder != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct proxy, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	port = GET_PORT(this, direction, port_id);

	while (true) {
		struct spa_pod *param;

		if (*index >= port->n_params)
			return 0;

		param = port->params[(*index)++];

		if (!spa_pod_is_object_id(param, id))
			continue;

		if (spa_pod_filter(builder, result, param, filter) == 0)
			break;
	}
	return 1;
}

static int
spa_proxy_node_port_set_param(struct spa_node *node,
			      enum spa_direction direction, uint32_t port_id,
			      uint32_t id, uint32_t flags,
			      const struct spa_pod *param)
{
	struct proxy *this;

	if (node == NULL)
		return -EINVAL;

	this = SPA_CONTAINER_OF(node, struct proxy, node);

	if (!CHECK_PORT(this, direction, port_id))
		return -EINVAL;

	if (this->resource == NULL)
		return 0;

	pw_client_node_resource_port_set_param(this->resource,
					       this->seq,
					       direction, port_id,
					       id, flags,
					       param);

	return SPA_RESULT_RETURN_ASYNC(this->seq++);
}

static int
spa_proxy_node_port_set_io(struct spa_node *node,
			   enum spa_direction direction, uint32_t port_id, struct spa_port_io *io)
{
	struct proxy *this;
	struct proxy_port *port;

	if (node == NULL)
		return -EINVAL;

	this = SPA_CONTAINER_OF(node, struct proxy, node);

	if (!CHECK_PORT(this, direction, port_id))
		return -EINVAL;

	port = GET_PORT(this, direction, port_id);
	port->io = io;

	return 0;
}

static int
spa_proxy_node_port_use_buffers(struct spa_node *node,
				enum spa_direction direction,
				uint32_t port_id,
				struct spa_buffer **buffers,
				uint32_t n_buffers)
{
	struct proxy *this;
	struct impl *impl;
	struct proxy_port *port;
	uint32_t i, j;
	size_t n_mem;
	struct pw_client_node_buffer *mb;
	struct spa_meta_shared *msh;
	struct pw_type *t;

	this = SPA_CONTAINER_OF(node, struct proxy, node);
	impl = this->impl;
	spa_log_info(this->log, "proxy %p: use buffers %p %u", this, buffers, n_buffers);

	t = impl->t;

	if (!CHECK_PORT(this, direction, port_id))
		return -EINVAL;

	port = GET_PORT(this, direction, port_id);

	if (!port->have_format)
		return -EIO;

	clear_buffers(this, port);

	if (n_buffers > 0) {
		mb = alloca(n_buffers * sizeof(struct pw_client_node_buffer));
	} else {
		mb = NULL;
	}

	port->n_buffers = n_buffers;

	if (this->resource == NULL)
		return 0;

	n_mem = 0;
	for (i = 0; i < n_buffers; i++) {
		struct proxy_buffer *b = &port->buffers[i];

		msh = spa_buffer_find_meta(buffers[i], t->meta.Shared);
		if (msh == NULL) {
			spa_log_error(this->log, "missing shared metadata on buffer %d", i);
			return -EINVAL;
		}

		b->outbuf = buffers[i];
		memcpy(&b->buffer, buffers[i], sizeof(struct spa_buffer));
		b->buffer.datas = b->datas;
		b->buffer.metas = b->metas;

		mb[i].buffer = &b->buffer;
		mb[i].mem_id = n_mem++;
		mb[i].offset = 0;
		mb[i].size = msh->size;

		pw_client_node_resource_port_add_mem(this->resource,
						     direction,
						     port_id,
						     mb[i].mem_id,
						     t->data.MemFd,
						     msh->fd, msh->flags, msh->offset, msh->size);

		for (j = 0; j < buffers[i]->n_metas; j++) {
			memcpy(&b->buffer.metas[j], &buffers[i]->metas[j], sizeof(struct spa_meta));
		}

		for (j = 0; j < buffers[i]->n_datas; j++) {
			struct spa_data *d = &buffers[i]->datas[j];

			memcpy(&b->buffer.datas[j], d, sizeof(struct spa_data));

			if (d->type == t->data.DmaBuf ||
			    d->type == t->data.MemFd) {
				pw_client_node_resource_port_add_mem(this->resource,
								     direction,
								     port_id,
								     n_mem,
								     d->type,
								     d->fd,
								     d->flags, d->mapoffset, d->maxsize);
				b->buffer.datas[j].type = t->data.Id;
				b->buffer.datas[j].data = SPA_UINT32_TO_PTR(n_mem);
				n_mem++;
			} else if (d->type == t->data.MemPtr) {
				b->buffer.datas[j].data = SPA_INT_TO_PTR(b->size);
				b->size += d->maxsize;
			} else {
				b->buffer.datas[j].type = SPA_ID_INVALID;
				b->buffer.datas[j].data = 0;
				spa_log_error(this->log, "invalid memory type %d", d->type);
			}
		}
	}

	pw_client_node_resource_port_use_buffers(this->resource,
						 this->seq,
						 direction, port_id,
						 n_buffers, mb);

	return SPA_RESULT_RETURN_ASYNC(this->seq++);
}

static int
spa_proxy_node_port_alloc_buffers(struct spa_node *node,
				  enum spa_direction direction,
				  uint32_t port_id,
				  struct spa_pod **params,
				  uint32_t n_params,
				  struct spa_buffer **buffers,
				  uint32_t *n_buffers)
{
	struct proxy *this;
	struct proxy_port *port;

	if (node == NULL || buffers == NULL)
		return -EINVAL;

	this = SPA_CONTAINER_OF(node, struct proxy, node);

	if (!CHECK_PORT(this, direction, port_id))
		return -EINVAL;

	port = GET_PORT(this, direction, port_id);

	if (!port->have_format)
		return -EIO;

	return -ENOTSUP;
}

static int
spa_proxy_node_port_reuse_buffer(struct spa_node *node, uint32_t port_id, uint32_t buffer_id)
{
	struct proxy *this;
	struct impl *impl;

	this = SPA_CONTAINER_OF(node, struct proxy, node);
	impl = this->impl;

	if (!CHECK_OUT_PORT(this, SPA_DIRECTION_OUTPUT, port_id))
		return -EINVAL;

	spa_log_trace(this->log, "reuse buffer %d", buffer_id);

	pw_client_node_transport_add_message(impl->transport, (struct pw_client_node_message *)
			&PW_CLIENT_NODE_MESSAGE_REUSE_BUFFER_INIT(port_id, buffer_id));

	return 0;
}

static int
spa_proxy_node_port_send_command(struct spa_node *node,
				 enum spa_direction direction,
				 uint32_t port_id, const struct spa_command *command)
{
	struct proxy *this;

	if (node == NULL || command == NULL)
		return -EINVAL;

	this = SPA_CONTAINER_OF(node, struct proxy, node);

	spa_log_warn(this->log, "unhandled command %d", SPA_COMMAND_TYPE(command));
	return -ENOTSUP;
}

static int spa_proxy_node_process_input(struct spa_node *node)
{
	struct proxy *this = SPA_CONTAINER_OF(node, struct proxy, node);
	struct impl *impl = this->impl;
	struct spa_graph_node *n = &impl->this.node->rt.node;
	bool client_reuse = impl->client_reuse;
	struct spa_graph_port *p, *pp;
	int res;

	if (impl->input_ready == 0) {
		/* the client is not ready to receive our buffers, recycle them */
		pw_log_trace("node not ready, recycle buffers");
		spa_list_for_each(p, &n->ports[SPA_DIRECTION_INPUT], link)
			p->io->status = SPA_STATUS_NEED_BUFFER;
		res = SPA_STATUS_NEED_BUFFER;
	}
	else {
		spa_list_for_each(p, &n->ports[SPA_DIRECTION_INPUT], link) {
			struct spa_port_io *io = p->io;

			pw_log_trace("set io status to %d %d", io->status, io->buffer_id);
			impl->transport->inputs[p->port_id] = *io;

			/* explicitly recycle buffers when the client is not going to do it */
			if (!client_reuse && (pp = p->peer))
		                spa_node_port_reuse_buffer(pp->node->implementation, pp->port_id, io->buffer_id);
		}
		pw_client_node_transport_add_message(impl->transport,
			       &PW_CLIENT_NODE_MESSAGE_INIT(PW_CLIENT_NODE_MESSAGE_PROCESS_INPUT));
		do_flush(this);

		impl->input_ready--;
		res = SPA_STATUS_OK;
	}
	return res;
}

static int spa_proxy_node_process_output(struct spa_node *node)
{
	struct proxy *this;
	struct impl *impl;
	struct spa_graph_node *n;
	struct spa_graph_port *p;

	this = SPA_CONTAINER_OF(node, struct proxy, node);
	impl = this->impl;
	n = &impl->this.node->rt.node;

	if (impl->out_pending)
		goto done;

	impl->out_pending = true;

	spa_list_for_each(p, &n->ports[SPA_DIRECTION_OUTPUT], link) {
		struct spa_port_io *io = p->io;

		impl->transport->outputs[p->port_id] = *io;

		pw_log_trace("%d %d -> %d %d", io->status, io->buffer_id,
				impl->transport->outputs[p->port_id].status,
				impl->transport->outputs[p->port_id].buffer_id);
	}

      done:
	pw_client_node_transport_add_message(impl->transport,
			       &PW_CLIENT_NODE_MESSAGE_INIT(PW_CLIENT_NODE_MESSAGE_PROCESS_OUTPUT));
	do_flush(this);

	return SPA_STATUS_OK;
}

static int handle_node_message(struct proxy *this, struct pw_client_node_message *message)
{
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, proxy);
	struct spa_graph_node *n;
	struct spa_graph_port *p;

	n = &impl->this.node->rt.node;

	if (PW_CLIENT_NODE_MESSAGE_TYPE(message) == PW_CLIENT_NODE_MESSAGE_HAVE_OUTPUT) {
		spa_list_for_each(p, &n->ports[SPA_DIRECTION_OUTPUT], link) {
			*p->io = impl->transport->outputs[p->port_id];
			pw_log_trace("have output %d %d", p->io->status, p->io->buffer_id);
		}
		impl->out_pending = false;
		this->callbacks->have_output(this->callbacks_data);
	} else if (PW_CLIENT_NODE_MESSAGE_TYPE(message) == PW_CLIENT_NODE_MESSAGE_NEED_INPUT) {
		spa_list_for_each(p, &n->ports[SPA_DIRECTION_INPUT], link) {
			*p->io = impl->transport->inputs[p->port_id];
			pw_log_trace("need input %d %d", p->io->status, p->io->buffer_id);
		}
		impl->input_ready++;
		this->callbacks->need_input(this->callbacks_data);
	} else if (PW_CLIENT_NODE_MESSAGE_TYPE(message) == PW_CLIENT_NODE_MESSAGE_REUSE_BUFFER) {
		if (impl->client_reuse) {
			struct pw_client_node_message_reuse_buffer *p =
			    (struct pw_client_node_message_reuse_buffer *) message;
			this->callbacks->reuse_buffer(this->callbacks_data, p->body.port_id.value,
						     p->body.buffer_id.value);
		}
	}
	return 0;
}

static void setup_transport(struct impl *impl)
{
	uint32_t max_inputs = 0, max_outputs = 0, n_inputs = 0, n_outputs = 0;

	spa_proxy_node_get_n_ports(&impl->proxy.node, &n_inputs, &max_inputs, &n_outputs, &max_outputs);

	impl->transport = pw_client_node_transport_new(max_inputs, max_outputs);
	impl->transport->area->n_input_ports = n_inputs;
	impl->transport->area->n_output_ports = n_outputs;
}

static void
client_node_done(void *data, int seq, int res)
{
	struct impl *impl = data;
	struct proxy *this = &impl->proxy;

	if (seq == 0 && res == 0)
		setup_transport(impl);

	this->callbacks->done(this->callbacks_data, seq, res);
}

static void
client_node_update(void *data,
		   uint32_t change_mask,
		   uint32_t max_input_ports,
		   uint32_t max_output_ports,
		   uint32_t n_params,
		   const struct spa_pod **params)
{
	struct impl *impl = data;
	struct proxy *this = &impl->proxy;

	if (change_mask & PW_CLIENT_NODE_UPDATE_MAX_INPUTS)
		this->max_inputs = max_input_ports;
	if (change_mask & PW_CLIENT_NODE_UPDATE_MAX_OUTPUTS)
		this->max_outputs = max_output_ports;
	if (change_mask & PW_CLIENT_NODE_UPDATE_PARAMS) {
		int i;
		spa_log_info(this->log, "proxy %p: update %d params", this, n_params);

		for (i = 0; i < this->n_params; i++)
			free(this->params[i]);
		this->n_params = n_params;
		this->params = realloc(this->params, this->n_params * sizeof(struct spa_pod *));

		for (i = 0; i < this->n_params; i++)
			this->params[i] = pw_spa_pod_copy(params[i]);
	}
	spa_log_info(this->log, "proxy %p: got node update max_in %u, max_out %u", this,
		     this->max_inputs, this->max_outputs);
}

static void
client_node_port_update(void *data,
			enum spa_direction direction,
			uint32_t port_id,
			uint32_t change_mask,
			uint32_t n_params,
			const struct spa_pod **params,
			const struct spa_port_info *info)
{
	struct impl *impl = data;
	struct proxy *this = &impl->proxy;
	bool remove;

	spa_log_info(this->log, "proxy %p: got port update", this);
	if (!CHECK_PORT_ID(this, direction, port_id))
		return;

	remove = (change_mask == 0);

	if (remove) {
		do_uninit_port(this, direction, port_id);
	} else {
		do_update_port(this,
			       direction,
			       port_id,
			       change_mask,
			       n_params, params, info);
	}
}

static void client_node_set_active(void *data, bool active)
{
	struct impl *impl = data;
	pw_node_set_active(impl->this.node, active);
}

static void client_node_event(void *data, struct spa_event *event)
{
	struct impl *impl = data;
	struct proxy *this = &impl->proxy;
	this->callbacks->event(this->callbacks_data, event);
}

static void client_node_destroy(void *data)
{
	struct impl *impl = data;
	pw_client_node_destroy(&impl->this);
}

static struct pw_client_node_proxy_methods client_node_methods = {
	PW_VERSION_CLIENT_NODE_PROXY_METHODS,
	.done = client_node_done,
	.update = client_node_update,
	.port_update = client_node_port_update,
	.set_active = client_node_set_active,
	.event = client_node_event,
	.destroy = client_node_destroy,
};

static void proxy_on_data_fd_events(struct spa_source *source)
{
	struct proxy *this = source->data;
	struct impl *impl = this->impl;

	if (source->rmask & (SPA_IO_ERR | SPA_IO_HUP)) {
		spa_log_warn(this->log, "proxy %p: got error", this);
		return;
	}

	if (source->rmask & SPA_IO_IN) {
		struct pw_client_node_message message;
		uint64_t cmd;

		if (read(this->data_source.fd, &cmd, sizeof(uint64_t)) != sizeof(uint64_t))
			spa_log_warn(this->log, "proxy %p: error reading message: %s",
					this, strerror(errno));

		while (pw_client_node_transport_next_message(impl->transport, &message) == 1) {
			struct pw_client_node_message *msg = alloca(SPA_POD_SIZE(&message));
			pw_client_node_transport_parse_message(impl->transport, msg);
			handle_node_message(this, msg);
		}
	}
}

static const struct spa_node proxy_node = {
	SPA_VERSION_NODE,
	NULL,
	spa_proxy_node_enum_params,
	spa_proxy_node_set_param,
	spa_proxy_node_send_command,
	spa_proxy_node_set_callbacks,
	spa_proxy_node_get_n_ports,
	spa_proxy_node_get_port_ids,
	spa_proxy_node_add_port,
	spa_proxy_node_remove_port,
	spa_proxy_node_port_get_info,
	spa_proxy_node_port_enum_params,
	spa_proxy_node_port_set_param,
	spa_proxy_node_port_use_buffers,
	spa_proxy_node_port_alloc_buffers,
	spa_proxy_node_port_set_io,
	spa_proxy_node_port_reuse_buffer,
	spa_proxy_node_port_send_command,
	spa_proxy_node_process_input,
	spa_proxy_node_process_output,
};

static int
proxy_init(struct proxy *this,
	   struct spa_dict *info,
	   const struct spa_support *support,
	   uint32_t n_support)
{
	uint32_t i;

	for (i = 0; i < n_support; i++) {
		if (strcmp(support[i].type, SPA_TYPE__Log) == 0)
			this->log = support[i].data;
		else if (strcmp(support[i].type, SPA_TYPE_LOOP__DataLoop) == 0)
			this->data_loop = support[i].data;
		else if (strcmp(support[i].type, SPA_TYPE__TypeMap) == 0)
			this->map = support[i].data;
	}
	if (this->data_loop == NULL) {
		spa_log_error(this->log, "a data-loop is needed");
		return -EINVAL;
	}
	if (this->map == NULL) {
		spa_log_error(this->log, "a type map is needed");
		return -EINVAL;
	}

	this->node = proxy_node;

	this->data_source.func = proxy_on_data_fd_events;
	this->data_source.data = this;
	this->data_source.fd = -1;
	this->data_source.mask = SPA_IO_IN | SPA_IO_ERR | SPA_IO_HUP;
	this->data_source.rmask = 0;

	return SPA_RESULT_RETURN_ASYNC(this->seq++);
}

static int proxy_clear(struct proxy *this)
{
	uint32_t i;

	for (i = 0; i < MAX_INPUTS; i++) {
		if (this->in_ports[i].valid)
			clear_port(this, &this->in_ports[i], SPA_DIRECTION_INPUT, i);
	}
	for (i = 0; i < MAX_OUTPUTS; i++) {
		if (this->out_ports[i].valid)
			clear_port(this, &this->out_ports[i], SPA_DIRECTION_OUTPUT, i);
	}

	return 0;
}

static void client_node_resource_destroy(void *data)
{
	struct impl *impl = data;
	struct pw_client_node *this = &impl->this;
	struct proxy *proxy = &impl->proxy;

	pw_log_debug("client-node %p: destroy", impl);

	impl->proxy.resource = this->resource = NULL;

	if (proxy->data_source.fd != -1)
		spa_loop_remove_source(proxy->data_loop, &proxy->data_source);

	pw_node_destroy(this->node);
}


static void node_initialized(void *data)
{
	struct impl *impl = data;
	struct pw_client_node *this = &impl->this;
	struct pw_node *node = this->node;

	if (this->resource == NULL)
		return;

	impl->fds[0] = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	impl->fds[1] = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	impl->proxy.data_source.fd = impl->fds[0];
	impl->proxy.writefd = impl->fds[1];
	impl->other_fds[0] = impl->fds[1];
	impl->other_fds[1] = impl->fds[0];

	spa_loop_add_source(impl->proxy.data_loop, &impl->proxy.data_source);
	pw_log_debug("client-node %p: add data fd %d", node, impl->proxy.data_source.fd);

	pw_client_node_resource_transport(this->resource, pw_global_get_id(pw_node_get_global(node)),
					  impl->other_fds[0], impl->other_fds[1], impl->transport);
}

static void node_free(void *data)
{
	struct impl *impl = data;

	pw_log_debug("client-node %p: free", &impl->this);
	proxy_clear(&impl->proxy);

	if (impl->transport)
		pw_client_node_transport_destroy(impl->transport);

	spa_hook_remove(&impl->node_listener);

	if (impl->fds[0] != -1)
		close(impl->fds[0]);
	if (impl->fds[1] != -1)
		close(impl->fds[1]);
	free(impl);
}

static const struct pw_node_events node_events = {
	PW_VERSION_NODE_EVENTS,
	.free = node_free,
	.initialized = node_initialized,
};

static const struct pw_resource_events resource_events = {
	PW_VERSION_RESOURCE_EVENTS,
	.destroy = client_node_resource_destroy,
};

/** Create a new client node
 * \param client an owner \ref pw_client
 * \param id an id
 * \param name a name
 * \param properties extra properties
 * \return a newly allocated client node
 *
 * Create a new \ref pw_node.
 *
 * \memberof pw_client_node
 */
struct pw_client_node *pw_client_node_new(struct pw_resource *resource,
					  struct pw_properties *properties)
{
	struct impl *impl;
	struct pw_client_node *this;
	struct pw_client *client = pw_resource_get_client(resource);
	struct pw_core *core = pw_client_get_core(client);
	const struct spa_support *support;
	uint32_t n_support;
	const char *name = "client-node";
	const char *str;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return NULL;

	this = &impl->this;

	impl->core = core;
	impl->t = pw_core_get_type(core);
	impl->fds[0] = impl->fds[1] = -1;
	pw_log_debug("client-node %p: new", impl);

	support = pw_core_get_support(impl->core, &n_support);

	proxy_init(&impl->proxy, NULL, support, n_support);
	impl->proxy.impl = impl;

	this->resource = resource;
	this->node = pw_spa_node_new(core,
				     pw_resource_get_client(this->resource),
				     NULL,
				     name,
				     PW_SPA_NODE_FLAG_ASYNC,
				     &impl->proxy.node,
				     NULL,
				     properties, 0);
	if (this->node == NULL)
		goto error_no_node;

	str = pw_properties_get(properties, "pipewire.client.reuse");
	impl->client_reuse = str && pw_properties_parse_bool(str);

	pw_resource_add_listener(this->resource,
				 &impl->resource_listener,
				 &resource_events,
				 impl);
	pw_resource_set_implementation(this->resource,
				       &client_node_methods,
				       impl);

	impl->proxy.resource = this->resource;

	pw_node_add_listener(this->node, &impl->node_listener, &node_events, impl);

	return this;

      error_no_node:
	pw_resource_destroy(this->resource);
	proxy_clear(&impl->proxy);
	free(impl);
	return NULL;
}

/** Destroy a client node
 * \param node the client node to destroy
 * \memberof pw_client_node
 */
void pw_client_node_destroy(struct pw_client_node *node)
{
	pw_resource_destroy(node->resource);
}

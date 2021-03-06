/* PipeWire
 * Copyright (C) 2017 Wim Taymans <wim.taymans@gmail.com>
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
#include <stdio.h>
#include <errno.h>
#include <dlfcn.h>

#include "config.h"

#include "pipewire/interfaces.h"
#include "pipewire/core.h"
#include "pipewire/log.h"
#include "pipewire/module.h"

#include "spa-node.h"

struct factory_data {
	struct pw_core *core;
	struct pw_factory *this;
	struct pw_properties *properties;
};

static void *create_object(void *_data,
			   struct pw_resource *resource,
			   uint32_t type,
			   uint32_t version,
			   struct pw_properties *properties,
			   uint32_t new_id)
{
	struct factory_data *data = _data;
	struct pw_node *node;
	const char *lib, *factory_name, *name;

	if (properties == NULL)
		goto no_properties;

	lib = pw_properties_get(properties, "spa.library.name");
	factory_name = pw_properties_get(properties, "spa.factory.name");
	name = pw_properties_get(properties, "name");

	if (lib == NULL || factory_name == NULL)
		goto no_properties;

	if (name == NULL)
		name = "spa-node";

	node = pw_spa_node_load(data->core,
				NULL,
				pw_factory_get_global(data->this),
				lib,
				factory_name,
				name,
				0,
				properties, 0);
	if (node == NULL)
		goto no_mem;

	if (resource)
		pw_global_bind(pw_node_get_global(node),
			       pw_resource_get_client(resource),
			       PW_PERM_RWX,
			       version, new_id);

	return node;

      no_properties:
	pw_log_error("needed properties: spa.library.name=<library-name> spa.factory.name=<factory-name>");
	if (resource) {
		pw_resource_error(resource, -EINVAL,
					"needed properties: "
						"spa.library.name=<library-name> "
						"spa.factory.name=<factory-name>");
	}
	return NULL;
      no_mem:
	pw_log_error("can't create node");
	if (resource) {
		pw_resource_error(resource, -ENOMEM, "no memory");
	}
	return NULL;
}

static const struct pw_factory_implementation impl_factory = {
	PW_VERSION_FACTORY_IMPLEMENTATION,
	.create_object = create_object,
};

static bool module_init(struct pw_module *module, struct pw_properties *properties)
{
	struct pw_core *core = pw_module_get_core(module);
	struct pw_type *t = pw_core_get_type(core);
	struct pw_factory *factory;
	struct factory_data *data;

	factory = pw_factory_new(core,
				 "spa-node-factory",
				 t->node,
				 PW_VERSION_NODE,
				 NULL,
				 sizeof(*data));
	if (factory == NULL)
		return false;

	data = pw_factory_get_user_data(factory);
	data->this = factory;
	data->core = core;
	data->properties = properties;

	pw_log_debug("module %p: new", module);

	pw_factory_set_implementation(factory,
				      &impl_factory,
				      data);

	pw_factory_register(factory, NULL, pw_module_get_global(module));

	return true;
}

#if 0
static void module_destroy(struct impl *impl)
{
	pw_log_debug("module %p: destroy", impl);

	free(impl);
}
#endif

bool pipewire__module_init(struct pw_module *module, const char *args)
{
	return module_init(module, NULL);
}

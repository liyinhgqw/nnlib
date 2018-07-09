
/*
 * Copyright (c) 2016-2018, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted (subject to the limitations in the
 * disclaimer below) provided that the following conditions are met:
 *
 *    * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *
 *    * Redistributions in binary form must reproduce the above
 *      copyright notice, this list of conditions and the following
 *      disclaimer in the documentation and/or other materials provided
 *      with the distribution.
 *
 *    * Neither the name of The Linux Foundation nor the names of its
 *      contributors may be used to endorse or promote products derived
 *      from this software without specific prior written permission.
 *
 * NO EXPRESS OR IMPLIED LICENSES TO ANY PARTY'S PATENT RIGHTS ARE
 * GRANTED BY THIS LICENSE. THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT
 * HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
 * IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

/*
 *
 * Now that that's out of the way, let's get to the good stuff.
 * 
 * This contains the code to append a node.
 */

#include <nn_graph.h>
#include <stdlib.h>
#include <stdio.h>

struct nn_node *alloc_node(uint32_t node_id, 
	op_type operation, padding_type padding)
{
	struct nn_node *newnode;
	if ((newnode = nn_malloc(sizeof(*newnode))) == NULL) {
		return newnode;
	}
	newnode->node_id = node_id;
	newnode->ops = optab[operation];
	newnode->node_type = operation;
	newnode->padding = padding;
	newnode->perfcounter = 0;
	newnode->executions = 0;
	newnode->opaque = NULL;
	return newnode;
}

static inline void free_inputs(struct nn_node *node)
{
	if (node->inputs) nn_free(node->inputs);
	if (node->input_refs) nn_free(node->input_refs);
}

const struct nn_node *get_node(struct nn_graph *nn, uint32_t id) {
	const struct nn_node *node = nn->head;
	while (node) {
		if (node->node_id == id) {
			return node;
		}
		node = node->next;
	}
	return NULL;
}
static inline int alloc_inputs(
	struct nn_graph *nn,
	struct nn_node *newnode, 
	uint32_t n, 
	const struct input *inputs)
{
	unsigned int tmpsize;
	int i;
	newnode->n_inputs = n;
	newnode->inputs = NULL;
	newnode->input_refs = NULL;
	if (n == 0) {
		return 0;
	}
	tmpsize = n*sizeof(newnode->input_refs[0]);
	/* allocate inputs */
	if ((newnode->input_refs = nn_calloc(1,tmpsize)) == NULL) {
		return errlog(nn,"input refs alloc failed");
	}
	if ((newnode->inputs = nn_calloc(n,sizeof(void *))) == NULL) {
		nn_free(newnode->input_refs);
		return errlog(nn,"input ptr storage alloc failed");
	}

	/* Copy input refs */
	for (i = 0; i < n; i++) {
		if (inputs[i].src_id == 0) {
			/* Or we could handle and dup tensor here */
			nn_free(newnode->input_refs);
			nn_free(newnode->inputs);
			return errlog(nn,"fatal: const tensor in generic input");
		}
		newnode->input_refs[i] = inputs[i];
		// Copy the shape from source to this input
		const struct nn_node *source_node = get_node(nn, inputs[i].src_id);
		if (source_node) {
			const struct tensor *source_output = source_node->outputs[inputs[i].output_idx];
			newnode->inputs[i] = source_output;
	}
	}
	return 0;
}

static inline void free_outputs(struct nn_node *node)
{
	int i;
	for (i = 0; i < node->n_outputs; i++) {
		node->outputs[i]->data = NULL;
		tensor_free(node->outputs[i]);
	}
	if (node->outputs) nn_free(node->outputs);
	if (node->output_defs) nn_free(node->output_defs);
}


static inline int alloc_outputs(
	struct nn_graph *nn,
	struct nn_node *newnode, 
	uint32_t n, 
	const struct output *outputs)
{
	int i;
	struct shape tshape;
	tshape.depth = tshape.width = tshape.height = tshape.batches = 0;
	newnode->n_outputs = n;
	if (n == 0) {
		newnode->outputs = NULL;
		newnode->output_defs = NULL;
		return 0;
	}
	/* Allocate outputs */
	if ((newnode->outputs = nn_calloc(n,sizeof(void *))) == NULL) {
		return errlog(nn,"output ptr storage alloc failed");
	}
	if ((newnode->output_defs = nn_calloc(n,sizeof(struct output))) == NULL) {
		nn_free(newnode->outputs);
		return errlog(nn,"output def storage alloc failed");
	}
	memcpy(newnode->output_defs,outputs,n*sizeof(struct output));
	/* Allocate outputs */
	/*
	 * Allocate base tensor struct but don't allocate storage until later.
	 * We could postpone longer, but this works pretty well.
	 */
	for (i = 0; i < n; i++) {
		if ((newnode->outputs[i] = tensor_alloc(&tshape,0)) == NULL) {
			goto err_free_allocated_outputs;
		}
		/* EJP: now unnecessary, we keep output shape definitions separate */
		newnode->outputs[i]->max_size = outputs[i].elementsize;
		newnode->outputs[i]->shape.batches = outputs[i].max_sizes[0];
		newnode->outputs[i]->shape.height = outputs[i].max_sizes[1];
		newnode->outputs[i]->shape.width = outputs[i].max_sizes[2];
		newnode->outputs[i]->shape.depth = outputs[i].max_sizes[3];
		for (uint32_t j=0; j<outputs[i].rank; j++) {
			newnode->outputs[i]->max_size *= outputs[i].max_sizes[j];
		}
		if (newnode->ops->flags & NN_NODE_FLAG_D32_OUTPUT) newnode->outputs[i]->max_size *= 2;
	}
	return 0;
err_free_allocated_outputs:
	for (i = 0; i < n; i++) {
		if (newnode->outputs[i]) tensor_free(newnode->outputs[i]);
	}
	nn_free(newnode->outputs);
	nn_free(newnode->output_defs);
	return errlog(nn,"output tensor malloc failed");
}

struct nn_node *node_alloc_common(
	struct nn_graph *nn,
	uint32_t node_id,
	op_type operation,
	padding_type padding,
	uint32_t num_inputs,
	uint32_t num_outputs,
	const struct input *inputs,
	const struct output *outputs)
{
	struct nn_node *newnode;
	if ((newnode = alloc_node(node_id,operation,padding)) == NULL) {
		errlog(nn,"common alloc id %x malloc fail",node_id);
		return NULL;
	}
	if (alloc_inputs(nn, newnode, num_inputs, inputs) != 0) {
		errlog(nn,"input alloc failed");
		goto err_free_node;
	}
	if (alloc_outputs(nn, newnode, num_outputs, outputs) != 0) {
		errlog(nn,"output alloc failed");
		goto err_free_inputs;
	}
	return newnode;
err_free_inputs:
	free_inputs(newnode);
err_free_node:
	nn_free(newnode);
	return NULL;
}

int node_free_common(struct nn_node *node, struct nn_graph *nn)
{
	logmsg(nn,3,"freeing node %p id=%x",node,node->node_id);
	free_inputs(node);
	free_outputs(node);
	del_node_from_hash(nn,node->node_id);
	nn_free(node);
	return 0;
}

//
// append newnode to end of linked-list
// 'ptr' points to nn->head, or to
// any node->next field of a node already in the list.
//
static inline void node_append(struct nn_node **head,
							   struct nn_node **tail,
							   struct nn_node *newnode) {
	newnode->next = NULL;
	if (*head == NULL) {
		*head = newnode;
	} else {
		(*tail)->next = newnode;
	}
	*tail = newnode;
}

int do_append_node(
	struct nn_graph *nn,
	uint32_t node_id,
	op_type operation,
	padding_type padding,
	uint32_t num_inputs,
	uint32_t num_outputs,
	const struct input *inputs,
	const struct output *outputs)
{
	/* Allocate new node */
	/* Set default parameters and ops */
	/* Call node->ctor(node) */
	struct nn_node *node;
	if ((node = optab[operation]->ctor(
		     nn,
		     node_id,
		     operation,
		     padding,
		     num_inputs,
		     num_outputs,
		     inputs,
		     outputs)) == NULL) {
		return errlog(nn,"node id=0x%x ctor fail",node_id);
	}
	node_append(&(nn->nonconst_head), &(nn->nonconst_tail), node);
	return 0;
}

extern struct nn_node *hexagon_nn_const_ctor(
	struct nn_graph *nn,
	uint32_t node_id,
	uint32_t batches,
	uint32_t height,
	uint32_t width,
	uint32_t depth,
	const uint8_t *data,
	uint32_t data_len);

extern struct nn_node *hexagon_nn_empty_const_ctor(
	struct nn_graph *nn,
	uint32_t node_id,
	uint32_t batches,
	uint32_t height,
	uint32_t width,
	uint32_t depth,
	uint32_t data_len);

extern int hexagon_nn_populate_const(
	struct nn_graph *nn,
	uint32_t node_id,
	const uint8_t *data,
	uint32_t data_len,
	uint32_t target_offset);

int do_append_const_node(
	struct nn_graph *nn,
	uint32_t node_id,
	uint32_t batches,
	uint32_t height,
	uint32_t width,
	uint32_t depth,
	const uint8_t *data,
	uint32_t data_len)
{
	/* Allocate new node */
	/* Set default parameters and ops */
	/* Call node->ctor(node) */
	struct nn_node *node;
	if ((node = hexagon_nn_const_ctor(
		nn,
		node_id,
		batches,
		height,
		width,
		depth,
		data,
		data_len)) == NULL) {
		return errlog(nn,"node id=0x%x ctor fail",node_id);
	}
	node_append(&(nn->head), &(nn->tail), node);
	return 0;
}

int do_append_empty_const_node(
	struct nn_graph *nn,
	uint32_t node_id,
	uint32_t batches,
	uint32_t height,
	uint32_t width,
	uint32_t depth,
	uint32_t data_len) {
	struct nn_node *node;
	if ((node = hexagon_nn_empty_const_ctor(
		     nn,
		     node_id,
		     batches,
		     height,
		     width,
		     depth,
		     data_len)) == NULL) {
		return errlog(nn,"node id=0x%x ctor fail",node_id);
	}
	node_append(&(nn->head), &(nn->tail), node);
	return 0;
}
int do_populate_const_node(
	struct nn_graph *nn,
	uint32_t node_id,
	const uint8_t *data,
	uint32_t data_len,
	uint32_t target_offset) {
	return hexagon_nn_populate_const(nn, node_id, data, data_len, target_offset);
}

int do_teardown(struct nn_graph *nn)
{
	struct nn_node *node;
	struct nn_node *nextnode;
	int err;
	nn_os_workers_kill(nn);
	nn->state = NN_GRAPH_INVALID;
	node = nn->head;
	while (node != NULL) {
		nextnode = node->next;
		if ((err = node->ops->dtor(node,nn)) != 0) {
			return errlog(nn,"dtor failed in teardown");
		}
		node = nextnode;
	}
	allocator_teardown(nn);
	find_node_teardown(nn);
	nn_free(nn->scratch);
	nn_free(nn->logbuf);
	if (nn->inputs) nn_free((void *)nn->inputs);
	if (nn->outputs) nn_free(nn->outputs);
	nn_free(nn);
	return 0;
}

//
// utilites for checking nodes
//  (can be called from 'check' functions)
//

// check if #inputs in range min_no .. max_no; and check non-null.
// if not, log error and return non-zero. "name" is the node name for error messages.
// max_no < 0 can be used to indicate that extra inputs may be NULL;
// e.g. min_no =2, max_no = -5 means inputs must be in range 2..5, and inputs 0,1 may not be
// null, but inputs 2,3,4 may be NULL; caller will need to check.
//

int node_check_inputs_range( struct nn_node *self, struct nn_graph *nn, char const *name, int32_t min_no, int32_t max_no)
{
	uint32_t n = self->n_inputs;
	uint32_t i;
	int maxabs = (max_no < 0)? -max_no: max_no;
	int nullcheck = (max_no < 0)? min_no: maxabs;
	if( nullcheck > n) nullcheck = n;

	if( n < min_no || n > maxabs )
		return errlog(nn, "%s: wrong # inputs %d (range: %d...%d)", name, n, min_no, max_no);
	if( nullcheck > 0){
		struct tensor const **inputs = self->inputs;
		if( inputs == NULL) return errlog(nn,"%s: input pointer is null", name);
		for( i = 0; i < nullcheck; i++){
			if ( inputs[i] == NULL)
				return errlog(nn,"%s: NULL input %d", name, i);
		}
	}
	return 0;
}
// check if #inputs =n; and check non-null.
// if not, log error and return non-zero. "name" is the node name for error messages.

int node_check_inputs_n( struct nn_node *self, struct nn_graph *nn, char const *name, int32_t n)
{
	return node_check_inputs_range(self,nn, name, n, n);	// should compile to move;jump
}
// check if #outputs in range min_no .. max_no; and check non-null.
// if not, log error and return non-zero. "name" is the node name for error messages.
// max_no < 0 can be used to indicate that extra outputs may be NULL;
// e.g. min_no =2, max_no = -5 means outputs must be in range 2..5, and outputs 0,1 may not be
// null, but inputs 2,3,4 may be NULL; caller will need to check.
//

int node_check_outputs_range( struct nn_node *self, struct nn_graph *nn, char const *name, int32_t min_no, int32_t max_no)
{
	uint32_t n = self->n_outputs;
	uint32_t i;
	int maxabs = (max_no < 0)? -max_no: max_no;
	int nullcheck = (max_no < 0)? min_no: maxabs;
	if( nullcheck > n) nullcheck = n;

	if( n < min_no || n > maxabs )
		return errlog(nn, "%s: wrong # outputs %d (range: %d...%d)", name, n, min_no, max_no);
	if( nullcheck > 0){
		struct tensor **outputs = self->outputs;
		if( outputs == NULL) return errlog(nn,"%s: output pointer is null", name);
		for( i = 0; i < nullcheck; i++){
			if (outputs[i] == NULL)
				return errlog(nn,"%s: NULL output %d", name, i);
		}
	}
	return 0;
}
// check if #outputs =n; and check non-null.
// if not, log error and return non-zero. "name" is the node name for error messages.

int node_check_outputs_n(  struct nn_node *self, struct nn_graph *nn, char const *name, int32_t n)
{
	return node_check_outputs_range(self,nn, name, n,n);
}

// check if #inputs = n_in, and outputs = n_out; and check non-null.
// if not, log error and return non-zero. "name" is the node name for error messages.
int node_check_inputs_outputs_n(  struct nn_node *self, struct nn_graph *nn, char const *name, int32_t n_in, int32_t n_out)
{
	int k = node_check_inputs_range( self,nn, name, n_in,n_in);
	if( k == 0 ) k = node_check_outputs_range( self,nn, name, n_out,n_out);
	return k;
}

void graphviz_print_node(struct nn_graph *nn, struct nn_node *node, FILE *dotfile)
{
#ifdef SHOWY_DEBUG
#ifdef LINUX_DEBUG
#define SAY_GRAPH(args...) printf(args);
#else
#define SAY_GRAPH(args...) fprintf(dotfile, args);
#endif
	if (nn == NULL || node == NULL) {
		return;
	}
	SAY_GRAPH("    MEM%p [label=\"%s id=%lu\"];\n",
            node, hexagon_nn_op_names[node->node_type], node->node_id);
	const struct tensor **t1 = node->inputs;
	if (t1 == NULL) {
	} else {
		for (int i=0; i<node->n_inputs; i++) {
			SAY_GRAPH("    MEM%p -> MEM%p [label=\"%d\"];\n", t1[i], node, i);
		}
	}
	struct tensor **t2 = node->outputs;
	if (t2 == NULL) {
	} else {
		for (int i=0; i<node->n_outputs; i++) {
			char *red = "red";
			char *blue = "blue";
			char *black = "black";
			char *color = black;
			int num_elements = t2[i]->shape.batches * t2[i]->shape.height *
				t2[i]->shape.width * t2[i]->shape.depth;
			if (t2[i]->max_size < num_elements) {
				color = red;
			} else if (t2[i]->max_size < (4 * num_elements)) {
				color = blue;
			}
			SAY_GRAPH("    MEM%p [shape=box,label=\"%lu*%lu*%lu*%lu %lu\",color=\"%s\"];\n",
				  t2[i], t2[i]->shape.batches, t2[i]->shape.height,
				  t2[i]->shape.width, t2[i]->shape.depth, t2[i]->max_size, color);
			SAY_GRAPH("    MEM%p -> MEM%p;\n", node, t2[i]);
		}
	}
#endif
}

void graphviz_print_graph(struct nn_graph *nn)
{
#ifdef SHOWY_DEBUG
        FILE *dotfile;
        char filename[255];

        if (nn == NULL) {
                return;
        }

        uint64_t pcycle = nn_os_get_cycles(NULL);

#ifndef LINUX_DEBUG
        snprintf(filename, 255, "debug/%llu_%p.dot\n", pcycle, nn);
        if ((dotfile = fopen(filename, "w")) == NULL) {
                printf("Ooops... Couldn't open file %s\n", filename);
                return;
        }
#endif

        struct nn_node *node = nn->head;

        SAY_GRAPH("digraph {\n");
        while (node) {
                graphviz_print_node(nn, node, dotfile);
                node = node->next;
        }
        SAY_GRAPH("}\n");
#ifndef LINUX_DEBUG
        fclose(dotfile);
#endif
#endif
}

void debug_print_node(struct nn_graph *nn, struct nn_node *node)
{
	if (nn == NULL || node == NULL || nn->debug_level < 2) {
		return;
	}
	logmsg(nn, 2, "Node %p type=%s id=%lu  %lu-in %lu-out",
	       node, hexagon_nn_op_names[node->node_type], node->node_id,
	       node->n_inputs, node->n_outputs);
	const struct tensor **t1 = node->inputs;
	if (t1 == NULL) {
		logmsg(nn, 2, "Node %p expected %d inputs, got nullptr",
		       node, node->n_inputs);
	} else {
		for (int i=0; i<node->n_inputs; i++) {
			logmsg(nn, 2, "Node INPUT %d@%p %lu*%lu*%lu*%lu %lu", i, t1[i],
			       t1[i]->shape.batches, t1[i]->shape.height,
			       t1[i]->shape.width, t1[i]->shape.depth, t1[i]->max_size);
		}
	}
	struct tensor **t2 = node->outputs;
	if (t2 == NULL) {
		logmsg(nn, 2, "Node %p expected %d outputs, got nullptr",
		       node, node->n_outputs);
	} else {
		for (int i=0; i<node->n_outputs; i++) {
			logmsg(nn, 2, "Node OUTPUT %d@%p %lu*%lu*%lu*%lu %lu", i, t2[i],
			       t2[i]->shape.batches, t2[i]->shape.height,
			       t2[i]->shape.width, t2[i]->shape.depth, t2[i]->max_size);
		}
	}
}

void debug_print_graph(struct nn_graph *nn)
{
	if (nn == NULL || nn->debug_level < 2) {
		return;
	}
	struct nn_node *node = nn->head;
	while (node) {
		debug_print_node(nn, node);
		node = node->next;
	}
}

uint32_t fletcher32_tensor(struct nn_graph *nn,   struct tensor *t )
{
	// TODO - JONWOLFE - Fix implicit assumption of elementsize=1
	size_t words = t->shape.batches * t->shape.height *
		t->shape.width * ((t->shape.depth + 1) / 2);

        uint32_t sum1 = 0xffff, sum2 = 0xffff;
        size_t tlen;

	uint32_t b=0;
	uint32_t h=0;
	uint32_t w=0;
	uint32_t d=0;
	uint8_t *data;
	uint16_t val;
        while (words) {
                tlen = ((words >= 359) ? 359 : words);
                words -= tlen;
                do {
			// Get the next data word from tensor,
			//   and deal with odd-length depths
			data = tensor_location_d32(t, b, h, w, d);
			if (d == t->shape.depth - 1) {
				// zero-pad the last word if depth is odd.
				val = *data << 8;
				d=0;
				w++;
			} else if (d == t->shape.depth - 2) {
				val = (*data << 8) + *(data + 1);
				d=0;
				w++;
			} else {
				val = (*data << 8) + *(data + 1);
				d+=2;
			}
			if (w == t->shape.width) {
				w=0;
				h++;
			}
			if (h == t->shape.height) {
				h=0;
				b++;
			}

			// Fletcher-32 maths
                        sum2 += sum1 += val;
                        tlen--;
                } while (tlen);
                sum1 = (sum1 & 0xffff) + (sum1 >> 16);
                sum2 = (sum2 & 0xffff) + (sum2 >> 16);
        }
        /* Second reduction step to reduce sums to 16 bits */
        sum1 = (sum1 & 0xffff) + (sum1 >> 16);
        sum2 = (sum2 & 0xffff) + (sum2 >> 16);

	if ( b != t->shape.batches || h || w || d ) {
		errlog(nn, "ERROR while computing checksum... Got to %d,%d,%d,%d on a %d,%d,%d,%d tensor",
		       b, h, w, d, t->shape.batches, t->shape.height,
		       t->shape.width, t->shape.depth);
		return 0; // Not a legal checksum... Some error.
	}
        return (sum2 << 16) | sum1;
}

void print_node_checksum(struct nn_graph *nn,  struct nn_node *node )
{
	if (nn == NULL ||node == NULL ||  nn->debug_level < 2) {
		return;
	}
	struct tensor **out = node->outputs;
	if (out == NULL) {
	} else {
		for (int i=0; i<node->n_outputs; i++) {
			uint32_t checksum = fletcher32_tensor(nn, out[i]);
			logmsg(nn, 2, "CHECKSUM: %p %08x", out[i], checksum);
		}
	}
	if (node->next) {
		debug_print_node(nn, node->next);
	}
}

void print_graph_checksum(struct nn_graph *nn)
{
	if (nn == NULL || nn->debug_level < 2) {
		return;
	}
	struct nn_node *node = nn->head;
	while (node) {
		print_node_checksum(nn, node);
		node = node->next;
	}
}

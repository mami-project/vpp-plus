/*
 * Copyright (c) 2015 Cisco and/or its affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <vlib/vlib.h>
#include <vnet/vnet.h>
#include <vnet/session/stream_session.h>
#include <vnet/pg/pg.h>
#include <vppinfra/error.h>
#include <latency/latency.h>
#include <latency/plus_packet.h>

/* Register the latency node */
vlib_node_registration_t latency_node;

/* Used to display LATENCY packets in the packet trace */
typedef struct {
  u16 src_port;
  u16 dst_port;
  u32 new_src_ip;
  u32 new_dst_ip;
  u16 type;
  u32 pkt_count;
} latency_trace_t;

/* packet trace format function */
static u8 * format_latency_trace (u8 * s, va_list * args) {
  /* Ignore two first arguments */
  CLIB_UNUSED (vlib_main_t * vm) = va_arg (*args, vlib_main_t *);
  CLIB_UNUSED (vlib_node_t * node) = va_arg (*args, vlib_node_t *);
  
  latency_trace_t * t = va_arg (*args, latency_trace_t *);

  const char * typeNames[] = {"TCP", "QUIC", "PLUS"};

  /* show LATENCY packet */
  s = format (s, "LATENCY packet: type: %s\n", typeNames[t->type]);
  s = format (s, "   src port: %u, dst port: %u\n", t->src_port, t->dst_port);
  s = format (s, "   (new) src ip: %u, (new) dst ip: %u\n", t->new_src_ip, t->new_dst_ip);
  s = format (s, "   pkt number in flow: %u\n", t->pkt_count);

  return s;
}

/* Current implementation does not drop any packets */
#define foreach_latency_error \
_(TEMP, "Currently not used")

typedef enum {
#define _(sym,str) LATENCY_ERROR_##sym,
  foreach_latency_error
#undef _
  LATENCY_N_ERROR,
} latency_error_t;


static char * latency_error_strings[] = {
#define _(sym,string) string,
  foreach_latency_error
#undef _
};

/* Protocols */
#define UDP_PROTOCOL 17
#define TCP_PROTOCOL 6

/* Header sizes in bytes */
#define SIZE_IP4 20
#define SIZE_UDP 8
#define SIZE_TCP 20
#define SIZE_QUIC_MIN 3
#define SIZE_PLUS 20
#define SIZE_PLUS_EXT_HELLO 3

/* QUIC bits */
#define IS_LONG 0x80
#define HAS_ID 0x40
#define KEY_FLAG 0x20
#define LATENCY_TYPE 0x1F
#define SIZE_TYPE 1

/* Only true for current pinq implementation (IETF draft 05)
   https://github.com/pietdevaere/minq */
#define P_NUMBER_8 0x01
#define P_NUMBER_16 0x02
#define P_NUMBER_32 0x03

#define SIZE_NUMBER_8 1
#define SIZE_NUMBER_16 2
#define SIZE_NUMBER_32 4

#define SIZE_ID 8
#define SIZE_VERSION 4
#define SIZE_LATENCY_SPIN 1

/* For reserved bits
 * spin in data_offset_and_reserved 00001110 */
#define TCP_LATENCY_MASK 0x0E
#define TCP_LATENCY_SHIFT 1

/* Timeout values (in 100ms) */
#define TIMEOUT 300

/* PLUS timeouts */
#define TO_IDLE 100
#define TO_ASSOCIATED 30
#define TO_STOP 20

/* We run before IP4_lookup node */
typedef enum {
  IP4_LOOKUP,
  LATENCY_N_NEXT,
} latency_next_t;

/**
 * @brief Main loop function
 * */
static uword
latency_node_fn (vlib_main_t * vm, vlib_node_runtime_t * node,
                 vlib_frame_t * frame) {
  
  u32 n_left_from, * from, * to_next;
  latency_next_t next_index;

  from = vlib_frame_vector_args (frame);
  n_left_from = frame->n_vectors;
  next_index = node->cached_next_index;

  while (n_left_from > 0) {

    u32 n_left_to_next;

    vlib_get_next_frame (vm, node, next_index,
                         to_next, n_left_to_next);

    /* Currently, only single loop implemented 
     * TODO: implement double loop */
    while (n_left_from > 0 && n_left_to_next > 0) {

      /* Advance timer wheel */
      expire_timers(vlib_time_now (vm));

      u32 bi0;
      vlib_buffer_t * b0;
      u32 next0 = 0;

      /* speculatively enqueue b0 to the current next frame */
      bi0 = from[0];
      to_next[0] = bi0;
      from += 1;
      to_next += 1;
      n_left_from -= 1;
      n_left_to_next -= 1;

      b0 = vlib_get_buffer (vm, bi0);

      /* Keeps track of all the buffer movement */
      u8 total_advance = 0;
      bool make_measurement = true;
      bool is_udp = true;

      /* Contains TCP, QUIC or PLUS session */
      latency_session_t * session = NULL;

      udp_header_t * udp0 = NULL;
      tcp_header_t * tcp0 = NULL;

      if (PREDICT_TRUE(b0->current_length >= SIZE_IP4)) {

        /* Get IP4 header */
        // TODO: add support for IP options and IPv6 headers
        ip4_header_t *ip0 = vlib_buffer_get_current(b0);
        vlib_buffer_advance (b0, SIZE_IP4);
        total_advance += SIZE_IP4;

        /* Ignore IPv6 packets */
        if (PREDICT_FALSE((ip0->ip_version_and_header_length & 0xF0) == 0x60)) {
          goto skip_packet;
        }

        if (ip0->protocol == UDP_PROTOCOL && b0->current_length >= SIZE_UDP) {
          /* Get UDP header */
          udp0 = vlib_buffer_get_current(b0);
          vlib_buffer_advance (b0, SIZE_UDP);
          total_advance += SIZE_UDP;
          is_udp = true;

          /* QUIC "detection", see if either endpoint is on the QUIC_PORT */
          if (is_quic(udp0->src_port, udp0->dst_port) &&
              b0->current_length >= SIZE_QUIC_MIN) {
              
            /* Get QUIC header */
            u64 connection_id;
            u32 packet_number, CLIB_UNUSED(latency_version);
            u8 *type = vlib_buffer_get_current(b0);

            /* LONG HEADER */
            /* We expect most packets to have the short header */
            if (PREDICT_FALSE(*type & IS_LONG)) {
              vlib_buffer_advance(b0, SIZE_TYPE);
              total_advance += SIZE_TYPE;

              /* Get connection ID */
              u64 *temp_id = vlib_buffer_get_current(b0);
              connection_id = clib_net_to_host_u64(*temp_id);
              vlib_buffer_advance(b0, SIZE_ID);
              total_advance += SIZE_ID;

              /* Get packet number PN */
              u32* temp_pn = vlib_buffer_get_current(b0);
              packet_number = clib_net_to_host_u32(*temp_pn);
              vlib_buffer_advance(b0, SIZE_NUMBER_32);
              total_advance += SIZE_NUMBER_32;

              /* Get version */
              u32 *temp_version = vlib_buffer_get_current(b0);
              latency_version = clib_net_to_host_u32(*temp_version);
              vlib_buffer_advance(b0, SIZE_VERSION);
              total_advance += SIZE_VERSION;

            /* SHORT HEADER */
            } else {
              vlib_buffer_advance (b0, SIZE_TYPE);
              total_advance += SIZE_TYPE;

              /* No latency version in the short header */
              latency_version = 0;

              /* Get connection ID */
              connection_id = 0;
              
              /* Only true for current pinq implementation (IETF draft 05)
               * For newest IETF draft (08) HAS_ID meaning is reversed */
              if (*type & HAS_ID && b0->current_length >= SIZE_ID) {
                u64 *temp_id = vlib_buffer_get_current(b0);
                connection_id = clib_net_to_host_u64(*temp_id);

                vlib_buffer_advance (b0, SIZE_ID);
                total_advance += SIZE_ID;
              }

              /* Get the packet number */
              switch (*type & LATENCY_TYPE) {
                case P_NUMBER_8:
                  if (PREDICT_TRUE(b0->current_length >= SIZE_NUMBER_8)) {
                    u8 *temp_8 = vlib_buffer_get_current(b0);
                    packet_number = *temp_8;
                    vlib_buffer_advance (b0, SIZE_NUMBER_8);
                    total_advance += SIZE_NUMBER_8;
                  } else {
                    goto skip_packet;
                  }
                  break;

                case P_NUMBER_16:
                  if (PREDICT_TRUE(b0->current_length >= SIZE_NUMBER_16)) {
                    u16 *temp_16 = vlib_buffer_get_current(b0);
                    packet_number = clib_net_to_host_u16(*temp_16);
                    vlib_buffer_advance (b0, SIZE_NUMBER_16);
                    total_advance += SIZE_NUMBER_16;
                  } else {
                    goto skip_packet;
                  }
                  break;

                case P_NUMBER_32:
                  if (PREDICT_TRUE(b0->current_length >= SIZE_NUMBER_32)) {
                    u32 *temp_32 = vlib_buffer_get_current(b0);
                    packet_number = clib_net_to_host_u32(*temp_32);
                    vlib_buffer_advance (b0, SIZE_NUMBER_32);
                    total_advance += SIZE_NUMBER_32;
                  } else {
                    goto skip_packet;
                  }
                  break;

                default:
                  goto skip_packet;
              }
            }

            u8 measurement;
            if (PREDICT_TRUE(b0->current_length >= SIZE_LATENCY_SPIN)) {
              u8 *temp_m = vlib_buffer_get_current(b0);
              measurement = *temp_m;
            } else {
              goto skip_packet;
            }

            latency_key_t kv;

            make_key(&kv, ip0->src_address.as_u32, ip0->dst_address.as_u32, udp0->src_port,
                     udp0->dst_port, ip0->protocol);

            /* Try to get a session for the key */
            session = get_session_from_key(&kv);

            /* Only for the first packet of a flow we do not have a matching session */
            if (PREDICT_FALSE(!session)) {
              
              /* Only consider flows for known dst (dst port) */
              u32 new_dst_ip;
              get_new_dst(&new_dst_ip, udp0->dst_port);
              if (!new_dst_ip) {
                goto skip_packet;
              }

              /* Create new session */
              u32 index = create_session(P_QUIC);
              session = get_latency_session(index);

              /* Save key for reverse lookup */
              session->key = kv.as_u64;

              /* Initialize values */
              session->quic->id = connection_id;
              session->init_src_port = udp0->src_port;
              session->init_src_ip = ip0->src_address.as_u32;
              session->new_dst_ip = new_dst_ip;
              
              update_state(&kv, session->index);

              /* Packets in reverse direction will get same session
               * Necessary because we rewrite the IPs */
              make_key(&kv, 0, new_dst_ip, udp0->src_port,
                       udp0->dst_port, ip0->protocol);
              update_state(&kv, session->index);
              
              session->key_reverse = kv.as_u64;
              
              session->pkt_count = 1;

              start_timer(session, TIMEOUT);
            }

            /* Do latency RTT estimation */
            update_quic_rtt_estimate(vm, session->quic, vlib_time_now (vm),
                          udp0->src_port, session->init_src_port, measurement,
                          packet_number, session->pkt_count);

          /* PLUS packet */
          } else {
            if (b0->current_length >= SIZE_PLUS) {
              plus_header_t *plus0 = vlib_buffer_get_current(b0);
              vlib_buffer_advance (b0, SIZE_PLUS);
              total_advance += SIZE_PLUS;
              if (PREDICT_TRUE((plus0->magic_and_flags & MAGIC_MASK) == MAGIC)) {
                latency_key_t kv;
                make_plus_key(&kv, ip0->src_address.as_u32, ip0->dst_address.as_u32,
                                udp0->src_port, udp0->dst_port, ip0->protocol,
                                plus0->CAT);
                
                session = get_session_from_key(&kv);

                if (PREDICT_FALSE(!session)) {

                  /* Only consider flows for known dst (dst port) */
                  u32 new_dst_ip;
                  get_new_dst(&new_dst_ip, udp0->dst_port);
                  if (!new_dst_ip) {
                    goto skip_packet;
                  }

                  /* Create new session */
                  u32 index = create_session(P_PLUS);
                  session = get_latency_session(index);

                  /* Save key for reverse lookup */
                  session->key = kv.as_u64;
                  session->plus->cat = plus0->CAT;

                  /* Initialize values */
                  session->init_src_port = udp0->src_port;
                  session->init_src_ip = ip0->src_address.as_u32;
                  session->new_dst_ip = new_dst_ip;
                  update_state(&kv, session->index);

                  /* Packets in reverse direction will get same session
                   * Necessary because we rewrite the IPs */
                  make_plus_key(&kv, 0, new_dst_ip, udp0->src_port,
                           udp0->dst_port, ip0->protocol,
                           plus0->CAT);

                  update_state(&kv, session->index);
                  
                  session->key_reverse = kv.as_u64;
                  
                  session->pkt_count = 1;

                  start_timer(session, TIMEOUT);
                }

                /* Do PLUS PSN PSE RTT estimation */
                update_plus_rtt_estimate(vm, session->plus, vlib_time_now (vm),
                              udp0->src_port, session->init_src_port,
                              clib_net_to_host_u32(plus0->PSN),
                              clib_net_to_host_u32(plus0->PSE),
                              clib_net_to_host_u64(plus0->CAT),
                              session->pkt_count);

                /* Handle extended header */
                plus_ext_hop_c_h_t *plus_ext_hop_c0;
                
                /* Enough space for extended header */
                if ((plus0->magic_and_flags & EXTENDED) && b0->current_length
                     >= SIZE_PLUS + SIZE_PLUS_EXT_HELLO) {
                  plus_ext_hop_c0 = vlib_buffer_get_current(b0);
                
                  u8 ii = plus_ext_hop_c0->PCF_len_and_II & 0x03;
                  /* "Hop count" header */
                  if (plus_ext_hop_c0->PCF_type == 1 && ii == 0) {
                    plus_ext_hop_c0->PCF_hop_c += 1;
                  }
                }
                
              }
            }
          }
        } else {
          /* TCP spin and TS */
          if (ip0->protocol == TCP_PROTOCOL && b0->current_length >= SIZE_TCP) {
                
            /* Get TCP header */
            tcp0 = vlib_buffer_get_current(b0);
            vlib_buffer_advance (b0, SIZE_TCP);
            total_advance += SIZE_TCP;
            is_udp = false;

            /* For timestamp values */
            u32 tsval = 0;
            u32 tsecr = 0;

            int parse_ret = tcp_options_parse_mod(tcp0, &tsval, &tsecr);

            if (parse_ret) {
              goto skip_packet;
            }

            /* Ignore SYN ACK packets, no VEC  */
            if (PREDICT_FALSE(tcp_syn(tcp0) && tcp_ack(tcp0))) {
              make_measurement = false;             
            }

            /* VEC data from reserved space */
            u8 measurement = (tcp0->data_offset_and_reserved & TCP_LATENCY_MASK)
                    >> TCP_LATENCY_SHIFT;

            latency_key_t kv;
            make_key(&kv, ip0->src_address.as_u32, ip0->dst_address.as_u32,
                     tcp0->src_port, tcp0->dst_port, ip0->protocol);

            session = get_session_from_key(&kv);

            /* Only first packet in a flow should not have a session */
            if (PREDICT_FALSE(!session)) {

              /* Only consider flows for known dst (dst port) */
              u32 new_dst_ip;
              get_new_dst(&new_dst_ip, tcp0->dst_port);
              if (!new_dst_ip) {
                goto skip_packet;
              }

              /* Create new session */
              u32 index = create_session(P_TCP);
              session = get_latency_session(index);

              /* Save key for reverse lookup */
              session->key = kv.as_u64;

              /* Initialize values */
              session->init_src_port = tcp0->src_port;
              session->init_src_ip = ip0->src_address.as_u32;
              session->new_dst_ip = new_dst_ip;
              update_state(&kv, session->index);

              /* Packets in reverse direction will get same session
               * Necessary because we rewrite the IPs */
              make_key(&kv, 0, new_dst_ip, tcp0->src_port,
                       tcp0->dst_port, ip0->protocol);
              update_state(&kv, session->index);
              
              session->key_reverse = kv.as_u64;
              
              session->pkt_count = 1;

              start_timer(session, TIMEOUT);
            }

            /* Do timestamp and latency RTT estimation */
            if (PREDICT_TRUE(make_measurement)) {
              update_tcp_rtt_estimate(vm, session->tcp, vlib_time_now (vm),
                        tcp0->src_port, session->init_src_port, measurement,
                        tsval, tsecr, session->pkt_count,
                        clib_net_to_host_u32(tcp0->seq_number));
            }
          }
        }

        if (!session) {
          goto skip_packet;
        }

        /* Keep track of packets for each flow */
        session->pkt_count ++;

        /* NAT-like IP translation */
        if (!ip_nat_translation(ip0, session->init_src_ip, session->new_dst_ip)) {
          goto skip_packet;
        }
        
        /* Update UDP and IP checksum */
        if (is_udp) {
          udp0->checksum = 0;
          udp0->checksum = ip4_tcp_udp_compute_checksum (vm, b0, ip0);
        } else {
          tcp0->checksum = 0;
          tcp0->checksum = ip4_tcp_udp_compute_checksum (vm, b0, ip0); 
        }
        ip0->checksum = ip4_header_checksum (ip0);

        /* Currently only ACTIVE and ERROR state
         * The timer is just used to free memory if flow is no longer observed
         * PLUS states not implemented at the moment */
        switch ((latency_state_t) session->state) {
          case LATENCY_STATE_ACTIVE:
            update_timer(session, TIMEOUT);
          break;

          case LATENCY_STATE_ERROR:
          break;

          default:
          break;
        }

        /* If packet trace is active */
        if (PREDICT_FALSE((node->flags & VLIB_NODE_FLAG_TRACE) 
            && (b0->flags & VLIB_BUFFER_IS_TRACED))) {
          
          latency_trace_t *t = vlib_add_trace (vm, node, b0, sizeof (*t));
          if (is_udp) {
            t->src_port = clib_net_to_host_u16(udp0->src_port);
            t->dst_port = clib_net_to_host_u16(udp0->dst_port);
          } else {
            t->src_port = clib_net_to_host_u16(tcp0->src_port);
            t->dst_port = clib_net_to_host_u16(tcp0->dst_port);
          }
          t->new_src_ip = clib_net_to_host_u32(ip0->src_address.as_u32);
          t->new_dst_ip = clib_net_to_host_u32(ip0->dst_address.as_u32);
          t->type = session->p_type;
          t->pkt_count = session->pkt_count;
        }

        /* Move buffer pointer back such that next node gets expected position */
skip_packet:
        vlib_buffer_advance (b0, -total_advance);
      }
      
      /* verify speculative enqueue, maybe switch current next frame */
      vlib_validate_buffer_enqueue_x1 (vm, node, next_index, to_next,
                                       n_left_to_next, bi0, next0);
    }

    vlib_put_next_frame (vm, node, next_index, n_left_to_next);
  }

  return frame->n_vectors;
}


VLIB_REGISTER_NODE (latency_node) = {
  .function = latency_node_fn,
  .name = "latency",
  .vector_size = sizeof (u32),
  .format_trace = format_latency_trace,
  .type = VLIB_NODE_TYPE_INTERNAL,
  
  .n_errors = ARRAY_LEN(latency_error_strings),
  .error_strings = latency_error_strings,

  .n_next_nodes = LATENCY_N_NEXT,

  /* Next node is the ip4-lookup node */
  .next_nodes = {
    [IP4_LOOKUP] = "ip4-lookup",
  },
};

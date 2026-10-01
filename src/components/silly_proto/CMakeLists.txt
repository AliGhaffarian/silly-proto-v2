idf_component_register(SRCS silly_proto.c emitter.c sender.c receiver.c node_state_machine.c token_passer.c
                    INCLUDE_DIRS include
                    REQUIRES uart_utils common_utils esp_timer linkedlist
                    )

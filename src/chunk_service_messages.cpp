#include <tornet/chunk_service_messages.hpp>
#include <fc/reflect_impl.hpp>
#include <fc/reflect_vector.hpp>

FC_REFLECT( tn::fetch_request, (target)(length)(offset) )
FC_REFLECT( tn::fetch_response, (result)(offset)(total_size)(data)(balance)(query_interval)(deadend_count) )
FC_REFLECT( tn::store_response, (result) )

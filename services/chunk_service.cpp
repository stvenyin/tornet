#include "chunk_session.hpp"
#include "reflect_chunk_session.hpp"
#include <tornet/error.hpp>
#include "chunk_service.hpp"
#include "chunk_search.hpp"
#include <boost/filesystem.hpp>
#include <boost/interprocess/file_mapping.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <fstream>
#include <scrypt/blowfish.hpp>
#include <scrypt/super_fast_hash.hpp>

#include <tornet/rpc/client.hpp>
#include <tornet/rpc/udt_connection.hpp>
#include <tornet/services/chunk_session.hpp>

using namespace tornet;
namespace chrono = boost::chrono;

chunk_service::chunk_service( const boost::filesystem::path& dbdir, const tornet::node::ptr& node, 
                              const std::string& name, uint16_t port, boost::cmt::thread* t )
:service(node,name,port,t),m_publishing(false) {
    boost::filesystem::create_directories(dbdir/"cache_db");
    boost::filesystem::create_directories(dbdir/"local_db");
    m_cache_db = boost::make_shared<tornet::db::chunk>( node->get_id(), dbdir/"cache_db" );
    m_cache_db->init();
    m_local_db = boost::make_shared<tornet::db::chunk>( node->get_id(), dbdir/"local_db" );
    m_local_db->init();
    m_pub_db = boost::make_shared<tornet::db::publish>( dbdir/"publish_db" );
    m_pub_db->init();
}


boost::any chunk_service::init_connection( const tornet::rpc::connection::ptr& con ) {
    boost::shared_ptr<chunk_session> cc(new chunk_session(m_cache_db,con) );
    boost::reflect::any_ptr<chunk_session> acc(cc);

    uint16_t mid = 0;
    boost::reflect::visit(acc, service::visitor<chunk_session>(*con, acc, mid) );

    return acc; 
}


/**
 *
 *
 */
void chunk_service::import( const boost::filesystem::path& infile, 
                            scrypt::sha1& tornet_id, scrypt::sha1& checksum,
                            const boost::filesystem::path& outfile ) 
{
  if( !boost::filesystem::exists( infile ) )
    TORNET_THROW( "File '%1%' does not exist.", %infile ); 
  if( boost::filesystem::is_directory( infile ) )
    TORNET_THROW( "'%1%' is a directory, expected a file.", %infile ); 
  if( !boost::filesystem::is_regular( infile ) )
    TORNET_THROW( "'%1%' is not a regular file.", %infile ); 

  uint64_t file_size  = boost::filesystem::file_size(infile);
  slog( "Importing %1% of %2% bytes", infile, file_size );
  if( file_size == 0 )
    TORNET_THROW( "'%1' is an empty file.", %infile );

  {
      using namespace boost::interprocess;
      file_mapping  mfile(infile.native().c_str(), boost::interprocess::read_only );
      mapped_region mregion(mfile,boost::interprocess::read_only,0,file_size);
      scrypt::sha1_hash( checksum, (char*)mregion.get_address(), mregion.get_size() );
  } 
  scrypt::blowfish bf;
  std::string fhstr = checksum;
  bf.start( (unsigned char*)fhstr.c_str(), fhstr.size() );
  bf.reset_chain();
  slog( "Checksum %1%", fhstr );

  std::ifstream in(infile.native().c_str(), std::ifstream::in | std::ifstream::binary );

  uint64_t rfile_size = ((file_size+7)/8)*8; // needs to be a power of 8 for FB
  uint64_t chunk_size = 1024*1024;
  std::vector<char> chunk( (std::min)(chunk_size,rfile_size) );

  tornet_file tf(infile.filename().native(),file_size);
  int64_t r = 0;

  while( r < file_size ) {
    int64_t c = (std::min)( uint64_t(file_size-r), (uint64_t)chunk.size() );
    if( c < chunk.size() ) 
      memset( &chunk.front() + c, 0, chunk.size()-c );
    in.read( &chunk.front(), c );
    int64_t pc = c;
    c = ((c+7)/8)*8;

    bf.encrypt((unsigned char*)&chunk.front(), c, scrypt::blowfish::CBC );

    scrypt::sha1 chunk_id;
    scrypt::sha1_hash(chunk_id, &chunk.front(), c );
    tf.chunks.push_back( tornet_file::chunk_data( pc, chunk_id ) );
    slog( "Chunk %1% id %2%", tf.chunks.size(), chunk_id );

    // 64KB slices to allow parallel requests
    int64_t s = 0;
    while( s < c ) {
     int64_t ss = (std::min)( int64_t(64*1024), int64_t(c-s) ); 
     tf.chunks.back().slices.push_back( scrypt::super_fast_hash( &chunk.front()+s, ss ) );
     s += ss;
    }
    // store the chunk in the database
    m_local_db->store_chunk( chunk_id, boost::asio::buffer( &chunk.front(), c ) );

    r += c;
  }
  tf.checksum = checksum;

  tornet::rpc::raw::pack_vec( chunk, tf );
  chunk.resize( ((chunk.size()+7)/8)*8 );
  bf.reset_chain();
  bf.encrypt( (unsigned char*)&chunk.front(), chunk.size(), scrypt::blowfish::CBC );

  scrypt::sha1_hash( tornet_id, &chunk.front(), chunk.size() );
  m_local_db->store_chunk( tornet_id, boost::asio::buffer( &chunk.front(), chunk.size() ) );

  std::string of;
  if( outfile == boost::filesystem::path() )
    of = infile.native() + ".tornet";
  else
    of = outfile.native();
  std::ofstream out( of.c_str(), std::ostream::binary | std::ostream::out );
  tornet::rpc::raw::pack( out, tf );
}

void chunk_service::export_tornet( const scrypt::sha1& tornet_id, const scrypt::sha1& checksum ) {
  tornet_file tf;
  fetch_tornet( tornet_id, checksum, tf );

  scrypt::blowfish bf;
  std::string fhstr = checksum;
  bf.start( (unsigned char*)fhstr.c_str(), fhstr.size() );

  // create a file of tf.size rounded up to the nearest 8 bytes (for blowfish)

  //int64_t rsize = ((tf.size+7)/8)*8;
  int64_t rsize = tf.size;
  FILE* f = fopen(tf.name.c_str(), "w+b");
  fseeko( f, rsize-1, SEEK_SET );
  char l=0;
  fwrite( &l, 1, 1, f );
  fflush(f);
  fclose(f);

  using namespace boost::interprocess;
  file_mapping  mfile(tf.name.c_str(), read_write );
  mapped_region mregion(mfile,read_write );

  bf.reset_chain();
  char* start = (char*)mregion.get_address();
  char* pos = start;
  char* end = pos + mregion.get_size();
  for( uint32_t i = 0; i < tf.chunks.size(); ++i ) {
    slog( "writing chunk %1% %5% at pos %2% size: %3%,   %4% remaining", i, uint64_t(pos-start), tf.chunks[i].size, uint64_t(end-pos), tf.chunks[i].id );
    if( (pos + tf.chunks[i].size) > end ) {
      TORNET_THROW( "Attempt to write beyond end of file!" );
    }
    int adj_size = ((tf.chunks[i].size+7)/8)*8;
    if( adj_size != tf.chunks[i].size ) {
      std::vector<char> tmp(adj_size);
      if( !m_local_db->fetch_chunk( tf.chunks[i].id, boost::asio::buffer(tmp) ) ) {
        TORNET_THROW( "Error fetching chunk %1%", %tf.chunks[i].id );
      }
      bf.decrypt( (unsigned char*)&tmp.front(), adj_size, scrypt::blowfish::CBC );
      memcpy( pos, &tmp.front(), tf.chunks[i].size );
    } else {
      if( !m_local_db->fetch_chunk( tf.chunks[i].id, boost::asio::buffer(pos,adj_size) ) ) {
        TORNET_THROW( "Error fetching chunk %1%", %tf.chunks[i].id );
      }
      //assert( rcheck == tf.chunks[i].id );
      bf.decrypt( (unsigned char*)pos, adj_size, scrypt::blowfish::CBC );
    }
    //scrypt::sha1 rcheck; scrypt::sha1_hash( rcheck, pos, adj_size );
    //slog( "decrypt sha1 %1%", rcheck );

    pos += tf.chunks[i].size;
  }

  //bf.decrypt( (unsigned char*)mregion.get_address(), rsize, scrypt::blowfish::CBC );
  scrypt::sha1 fcheck;
  scrypt::sha1_hash( fcheck, (char*)mregion.get_address(), tf.size );
  if( fcheck != checksum ) {
    TORNET_THROW( "File checksum mismatch, got %1% expected %2%", %fcheck %checksum );
  }
}

void chunk_service::fetch_chunk( const scrypt::sha1& chunk_id, std::vector<char>& d ) {
  tornet::db::chunk::meta met;
  if( m_local_db->fetch_meta( chunk_id, met, false ) ) {
      d.resize(met.size);
      if( m_local_db->fetch_chunk( chunk_id, boost::asio::buffer(d) ) )
        return;
  }
  if( m_cache_db->fetch_meta( chunk_id, met, false ) ) {
      d.resize(met.size);
      if( m_cache_db->fetch_chunk( chunk_id, boost::asio::buffer(d) ) ) 
        return;
  }
  TORNET_THROW( "Unknown chunk %1%", %chunk_id );
}

/**
 *  Fetch and decode the tornet file description, but not the chunks
 */
void chunk_service::fetch_tornet( const scrypt::sha1& tornet_id, const scrypt::sha1& checksum, tornet_file& tf ) {
  tornet::db::chunk::meta met;
  if( m_local_db->fetch_meta( tornet_id, met, false ) ) {
      std::vector<char> tnet(met.size);
      if( m_local_db->fetch_chunk( tornet_id, boost::asio::buffer(tnet) ) ) {
          scrypt::blowfish bf;
          std::string fhstr = checksum;
          bf.start( (unsigned char*)fhstr.c_str(), fhstr.size() );
          bf.decrypt( (unsigned char*)&tnet.front(), tnet.size(), scrypt::blowfish::CBC );

          scrypt::sha1 check;
          tornet::rpc::raw::unpack_vec(tnet, check );
          if( check != checksum ) {
            TORNET_THROW( "Checksum mismatch, got %1% expected %2%", %check %checksum );
          }

          tornet::rpc::raw::unpack_vec(tnet, tf );
          if( tf.checksum != checksum ) {
            TORNET_THROW("Checksum mismatch, got %1% tornet file said %2%", %checksum %tf.checksum );
          } else {
            slog( "Decoded checksum %1%", tf.checksum );
          }
          slog( "File name: %1%  size %2%", tf.name, tf.size );
          return;
      } else { 
        TORNET_THROW( "Unknown to find data for chunk %1%", %tornet_id );
      }
  } else {
    TORNET_THROW( "Unknown chunk %1%", %tornet_id );
  }
}

/**
 *  This method should fetch the tornet file from local storage and then decode the chunks.
 *
 *  To publish a chunk search for the N nearest nodes and track the availability of that
 *  chunk.  If less than the desired number of hosts are found, then pick the host closest
 *  to the chunk and upload a copy.  If the desired number of hosts are found simply note
 *  the popularity of that chunk and schedule a time to check again.
 *
 *
 *
 */
void chunk_service::publish_tornet( const scrypt::sha1& tid, const scrypt::sha1& cs, uint32_t rep ) {
  tornet_file tf;
  fetch_tornet( tid, cs, tf );
  for( uint32_t i = 0; i < tf.chunks.size(); ++i ) {
    //if( !m_local_db->exists(tf.chunks[i]) && !m_cache_db->exists(tf.chunks[i] ) ) {
    //  TORNET_THROW( "Unable to publish tornet file because not all chunks are known to this node." );
    //  // TODO: Should we publish the parts we know?  Should we attempt to fetch the parts we don't?
   // }
    tornet::db::publish::record rec;
    m_pub_db->fetch( tf.chunks[i].id, rec );
    rec.desired_host_count = rep;
    rec.next_update        = 0;
    m_pub_db->store( tf.chunks[i].id, rec );
  }
  tornet::db::publish::record rec;
  m_pub_db->fetch( tid, rec );
  rec.desired_host_count = rep;
  rec.next_update        = 0;
  m_pub_db->store( tid, rec );
}

void chunk_service::enable_publishing( bool state ) {
  if( &boost::cmt::thread::current() != get_thread() ) {
    get_thread()->sync( boost::bind( &chunk_service::enable_publishing, this, state ) );
    return;
  }
  if( state != m_publishing ) {
      m_publishing = state;
      slog( "state %1%", state );
      if( state ) { 
        wlog( "async!" );
        get_thread()->async( boost::bind( &chunk_service::publish_loop, this ) );
      } else {
      }
  }
}

bool chunk_service::publishing_enabled()const { 
  return m_publishing;
}


/**
 *  
 *
 */
void chunk_service::publish_loop() {
  slog( "publish loop" );
  while( m_publishing ) {
    // find the next chunk that needs to be published
    scrypt::sha1    chunk_id;
    tornet::db::publish::record next_pub;
    if ( m_pub_db->fetch_next( chunk_id, next_pub ) ) {
       int64_t time_till_update = next_pub.next_update - 
                        boost::chrono::duration_cast<boost::chrono::microseconds>
                        (boost::chrono::system_clock::now().time_since_epoch()).count();
       if( time_till_update > 0 ) {
        slog( "waiting %1% us for next publish update.", -time_till_update );
        boost::cmt::usleep( time_till_update );
       }

       // Attempt a KAD lookup for the chunk, find up to 2x desired hosts, using parallelism of 1
       // TODO: increase parallism of search?? This should be a background task so latency is not an issue... 
       tornet::chunk_search::ptr csearch(  boost::make_shared<tornet::chunk_search>(get_node(), chunk_id, next_pub.desired_host_count*2, 1, true ) );  
       csearch->start();
       csearch->wait();

      typedef std::map<node::id_type,node::id_type> chunk_map;
      const chunk_map&  hn = csearch->hosting_nodes();

      // If the number of nodes hosting the chunk < next_pub.desired_host_count
      if( hn.size() < next_pub.desired_host_count ) {
        wlog( "Published chunk %1% found on at least %2% hosts, desired replication is %3%",
             chunk_id, hn.size(), next_pub.desired_host_count );

        slog( "Hosting nodes: " );
        chunk_map::const_iterator itr = hn.begin();
        while( itr != hn.end() ) {
          slog( "    node-dist: %1%  node id: %2%", itr->first, itr->second );
          ++itr;
        }
        slog( "Near nodes: " );
        const chunk_map&  nn = csearch->current_results();
        itr = nn.begin();
        while( itr != nn.end() ) {
          slog( "    node-dist: %1%  node id: %2%", itr->first, itr->second );
          ++itr;
        }
        itr = nn.begin();

       //   Find the closest node not hosting the chunk and upload the chunk
       while( itr != nn.end() && itr->second == get_node()->get_id() ) {
        ++itr;
       }
       if( itr == nn.end() ) {
         elog( "No hosts to publish to!" );
       } else {
         tornet::rpc::client<chunk_session>&  chunk_client = 
          *tornet::rpc::client<chunk_session>::get_udt_connection( get_node(), itr->second );
        
         std::vector<char> chunk_data;
         fetch_chunk( chunk_id, chunk_data );
         slog( "Uploading chunk... size %1% bytes", chunk_data.size() );
         store_response r = chunk_client->store( chunk_data ).wait();
         slog( "Response: %1%  balance: %2%", int(r.result), r.balance );
       }

       //   Wait until the upload has completed 
       //     update the next_pub host count and update time
       //     continue the publishing loop.
       //
       //   TODO: enable publishing N chunks in parallel 
      } else {
        slog( "Published chunk %1% found on at least %2% hosts, desired replication is %3%",
             chunk_id, hn.size(), next_pub.desired_host_count );

      }

      // TODO: calculate next update interval for this chunk based upon its current popularity
      //       and host count and how far away the closest host was found.
      uint64_t next_update = 60*1000*1000;
      next_pub.next_update = 
        chrono::duration_cast<chrono::microseconds>( chrono::system_clock::now().time_since_epoch()).count() + next_update;

      m_pub_db->store( chunk_id, next_pub );
    } else {
      // TODO: do something smarter, like exit publish loop until there is something to publish
      boost::cmt::usleep( 1000 * 1000  );
      wlog( "nothing to publish..." );
    }
//    elog( "wait to send next" );
//    boost::cmt::usleep(1000*1000);
//    elog( "wait to send next" );
  }
}

#if 0
void publish_loop() {
  db::publish::ptr pdb;

  while( publishing ) {
    scrypt::sha1 cid, nid;
    db::publish::record rec;
    pdb->fetch_oldest( cid, nid, rec );

    // get results

    // find 
    tornet::chunk_search::ptr cs( new tornet::chunk_search( node, cid ) );
    cs->start();
    cs->wait();
  
    const std::map<tornet::node::id_type,tornet::node::id_type>&  r = ks->current_results();
    std::map<tornet::node::id_type,tornet::node::id_type>::const_iterator itr  = r.begin(); 
    while( itr != r.end() ) {
      // total weighted access interval
      // pdb->store( cid, nid, now, normalize(access_interval,node distance) );
      ++itr;
    }
    pdb->store( cid, publish::record( now, avg_access_rate, r.size() ) );

    if( r.size() < rec.desired_host_count ) {
      cs->best_node_to_publish_to()->publish( chunk );
    }
  }
}
#endif



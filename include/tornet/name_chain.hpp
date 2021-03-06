#pragma once
#include <fc/reflect.hpp>
#include <fc/pke.hpp>
#include <fc/sha1.hpp>
#include <fc/vector.hpp>
#include <fc/string.hpp>
#include <fc/array.hpp>
#include <fc/raw.hpp>

#include <cafs.hpp>

namespace tn {
  /**
   *   The name block chain never stores transactions for blocks over 6 months old which
   *   means only the block headers must be stored.
   *
   *   All names / updates / revisions older than 6 months automatically expire.  In order
   *   to publish an update or transfer a name, 
   *
   *
   */




  /**
   *  Before a user can publish a transaction, the
   *  must find a hash for their transaction that is
   *  below some minimal threshold to show proof of
   *  work.  
   *
   *  The act of finding hashes for your transaction also
   *  helps you find hashes to solve the block because the
   *  hash of your transaction contains the hash of the
   *  block header.  
   *
   *  The difficulty of the block is the sum of all transaction
   *  difficulties.  To win the block you must include the most
   *  transactions from other users.  Failure to do so means that
   *  someone else could 'bump' your block and you would have to
   *  restart solving for your transaction.
   *
   *  The difficulty of finding a block is adjusted every block such
   *  that on average a new block is found every 10 minutes.  
   *
   *  The difficulty of finding a block is never less than the difficluty
   *  of solving a transaction which for an average computer should take
   *  1 hour of CPU time.   In effect, until the user base grows, a new
   *  block will be issued for every transaction.
   *
   *  The timestamp on the block must be more than 8 minutes after the 
   *  previous block timestamp AND must be less than current UTC for the
   *  block to be accepted.   So you cannot game the system by calculating
   *  on a 'future time' nor create a block faster than every 8 minutes.
   *  
   */
  struct name_block {
    name_block()
    :utc_us(0),block_num(0),block_date(0){}

    /// hash of these values form the base
    fc::sha1              prev_block_id;
    fc::time_point        utc_us;  // approx time the block was generated  
    uint64_t              block_num; 
    fc::vector<fc::sha1>  transactions; 

    fc::sha1 base_hash()const {
      fc::sha1::encoder enc;
      fc::raw::pack( enc, prev_block_id );
      fc::raw::pack( enc, utc_us );
      fc::raw::pack( enc, block_num );
      fc::raw::pack( enc, transactions );
      return enc.result();
    }
    
    uint64_t difficulty() {
      return transactions.size();
    }

    /// this is the transaction that solved the block.
    fc::sha1              gen_transaction;
  };


  template<typename Trx>
  bool validate_trx_hash( const Trx& tran, const fc::sha1& thresh ) {
     fc::sha1::encoder enc;
     fc::raw::pack( enc, tran );
     return  enc.result() < thresh;
  }

  /**
   *  For a block hash to be valid, the gen_transaction must use 
   *  the hash of the base header fields for is 'base' and the
   *  hash of the transaction must be below block thresh
   */
  template<typename Trx>
  bool validate_block_hash( const name_block& b, const Trx& gen, const fc::sha1& block_thresh, const fc::sha1& trx_thresh ) {
     if( gen.head.base != b.base_hash() ) 
        return false;

     // make sure that gen is really the one used.
     fc::sha1::encoder enc;
     fc::raw::pack( enc, gen );
     
     if( b.gen_transaction != enc.result() )
        return false;


     if( enc.result() >= block_thresh ) return false;
     
     // every trx must be below the desired trx_thresh
     for( auto itr = b.transactions.begin(); itr != b.transactions.end(); ++itr )
        if( *itr > trx_thresh ) return false;

     return true;
  }

  
  /**
   *  Searches for a nonce that will make the transaction hash below thresh.
   *  @param start1 where to start searching for the nonce
   *  @param done   a volatile boolean that can be used to exit the search early
   */
  template<typename Trx>
  uint64_t find_nonce( Trx& tran, uint64_t start, uint64_t end, const fc::sha1& thresh, volatile bool& done  ) {
    tran.head.nonce = start;
    while( !done && tran.head.nonce < end ) {
     if( validate_trx_hash( tran, thresh ) )
        return start;
      tran.head.nonce++;
    }
    return start;
  }

}

FC_REFLECT( tn::name_trx_header,   (base)(nonce)(type)(signature) )
FC_REFLECT( tn::name_reserve_trx,  (head)(pub_key)(res_id) )
FC_REFLECT( tn::name_publish_trx,  (head)(name)(rand)(site_ref) )
FC_REFLECT( tn::name_update_trx,   (head)(name_id)(update_count)(site_ref) )
FC_REFLECT( tn::name_transfer_trx, (head)(name_id)(to_pub_key) )
FC_REFLECT( tn::name_block,        (prev_block_id)(utc_us)(block_num)(transactions)(gen_transaction) )

#endif

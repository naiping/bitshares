/** This file is designed to be included in detail::chain_database_impl, but is pulled into a separate file for
 * ease of maitenance and upgrade.
 */

class market_engine
{
   public:
      market_engine( pending_chain_state_ptr ps, chain_database_impl& cdi )
      :_pending_state(ps),_db_impl(cdi)
      {
          _pending_state = std::make_shared<pending_chain_state>( ps );
          _prior_state = ps;
      }

      void execute( asset_id_type quote_id, asset_id_type base_id, const fc::time_point_sec& timestamp )
      {
         try {
             _quote_id = quote_id;
             _base_id = base_id;
             auto quote_asset = _pending_state->get_asset_record( _quote_id );
             auto base_asset = _pending_state->get_asset_record( _base_id );

             // DISABLE MARKET ISSUED ASSETS
             if( quote_asset->is_market_issued() )
             {
             //   return; // don't execute anything.
             }


             // the order book is soreted from low to high price, so to get the last item (highest bid), we need to go to the first item in the
             // next market class and then back up one
             auto next_pair  = base_id+1 == quote_id ? price( 0, quote_id+1, 0) : price( 0, quote_id, base_id+1 );
             _bid_itr        = _db_impl._bid_db.lower_bound( market_index_key( next_pair ) );
             _ask_itr        = _db_impl._ask_db.lower_bound( market_index_key( price( 0, quote_id, base_id) ) );
             _short_itr      = _db_impl._short_db.lower_bound( market_index_key( next_pair ) );
             _collateral_itr = _db_impl._collateral_db.lower_bound( market_index_key( next_pair ) );
       
             if( !_ask_itr.valid() )
             {
                wlog( "ask iter invalid..." );
                _ask_itr = _db_impl._ask_db.begin();
             }
       
             if( _short_itr.valid() ) --_short_itr;
             else _short_itr = _db_impl._short_db.last();
       
             if( _bid_itr.valid() )   --_bid_itr;
             else _bid_itr = _db_impl._bid_db.last();
       
             if( _collateral_itr.valid() )   --_collateral_itr;
             else _collateral_itr = _db_impl._collateral_db.last();
       
             asset consumed_bid_depth(0,base_id);
             asset consumed_ask_depth(0,base_id);
       
             asset trading_volume(0, base_id);
       
             omarket_status market_stat = _pending_state->get_market_status( _quote_id, _base_id );
             if( !market_stat ) market_stat = market_status( quote_id, base_id, 0, 0 );

             price max_short_bid;
             price min_cover_ask;

             // while bootstraping we use this metric
             auto median_price = _db_impl.self->get_median_delegate_price( quote_id );

             // convert any fees collected in quote unit to XTS 
             if( base_id == 0 )
             {
                if( quote_asset->is_market_issued() )
                {
                   if( !median_price )
                      FC_CAPTURE_AND_THROW( insufficient_feeds, (quote_id) );
                   auto feed_max_short_bid = *median_price;
                   feed_max_short_bid.ratio *= 4;
                   feed_max_short_bid.ratio /= 3;

                   auto feed_min_ask = *median_price;
                   feed_min_ask.ratio *= 2;
                   feed_min_ask.ratio /= 3;

                   max_short_bid = feed_max_short_bid; //std::min( market_stat->maximum_bid(), feed_max_short_bid );
                   min_cover_ask = feed_min_ask; //std::max( market_stat->minimum_ask(), feed_min_ask );
                   edump( (max_short_bid)(min_cover_ask) );
                }

                wlog( "==========================  LIQUIDATE FEES ${amount}  =========================\n", ("amount", quote_asset->collected_fees) );

                get_next_bid(); // this is necessary for get_next_ask to work with collateral
                while( get_next_ask() && quote_asset->collected_fees > 0 )
                {
                   idump( (_current_ask) );
                   market_transaction mtrx;
                   mtrx.bid_price = _current_ask->get_price();
                   mtrx.ask_price = _current_ask->get_price();
                   mtrx.bid_owner = address();
                   mtrx.ask_owner = _current_ask->get_owner();
                   mtrx.bid_type  = bid_order;
                   mtrx.ask_type  = _current_ask->type;

                   auto ask_quote_quantity = _current_ask->get_quote_quantity();
                   auto quote_quantity_usd = std::min( quote_asset->collected_fees, ask_quote_quantity.amount );
                   mtrx.ask_received = asset(quote_quantity_usd,quote_id);
                   mtrx.ask_paid     = mtrx.ask_received * mtrx.ask_price;
                   mtrx.bid_paid     = mtrx.ask_received;
                   mtrx.bid_received = mtrx.ask_paid; // these get directed to accumulated fees

                   // mtrx.fees_collected = mtrx.ask_paid;

                   if( mtrx.ask_paid.amount == 0 )
                      break;

                   push_market_transaction(mtrx);

                   if( mtrx.ask_type == ask_order )
                      pay_current_ask( mtrx, *base_asset );
                   else
                      pay_current_cover( mtrx, *quote_asset );

                   market_stat->ask_depth -= mtrx.ask_paid.amount;

                   quote_asset->collected_fees -= mtrx.bid_paid.amount;
                   _pending_state->store_asset_record(*quote_asset);
                   _pending_state->store_asset_record(*base_asset);
                   // TODO: pay XTS to delegates
                   auto prev_accumulated_fees = _pending_state->get_accumulated_fees();
                   _pending_state->set_accumulated_fees( prev_accumulated_fees + mtrx.ask_paid.amount );
                }
                wlog( "==========================  DONE LIQUIDATE FEES BALANCE: ${amount}=========================\n", ("amount", quote_asset->collected_fees) );
             }
             edump( (_current_bid) );
             edump( (_current_ask) );

             while( get_next_bid() && get_next_ask() )
             {
                idump((_current_bid) );
                idump((_current_ask) );

                auto bid_quantity_xts = _current_bid->get_quantity();
                auto ask_quantity_xts = _current_ask->get_quantity();

                asset xts_paid_by_short( 0, base_id );
                asset current_bid_balance  = _current_bid->get_balance();
                asset current_ask_balance  = _current_ask->get_balance();

                /** the market transaction we are filling out */
                market_transaction mtrx;
                mtrx.bid_price = _current_bid->get_price();
                mtrx.ask_price = _current_ask->get_price();
                mtrx.bid_owner = _current_bid->get_owner();
                mtrx.ask_owner = _current_ask->get_owner();
                mtrx.bid_type  = _current_bid->type;
                mtrx.ask_type  = _current_ask->type;

                if( _current_ask->type == cover_order && _current_bid->type == short_order )
                {
                   elog( "CURRENT ASK IS COVER" );
                   FC_ASSERT( quote_asset->is_market_issued() && base_id == 0 );
                   if( mtrx.ask_price < mtrx.bid_price ) // the call price has not been reached
                      break;

                   // in the event that there is a margin call, we must accept the
                   // bid price assuming the bid price is reasonable 
                   if( mtrx.bid_price < min_cover_ask )
                   {
                      wlog( "skipping cover ${x} < min_cover_ask ${b}", ("x",_current_ask->get_price())("b", min_cover_ask)  );
                      _current_ask.reset();
                      continue;
                   }
                   mtrx.ask_price = mtrx.bid_price;

                   // we want to sell enough XTS to cover our balance.
                   ask_quantity_xts  = current_ask_balance * mtrx.ask_price;
                   auto quantity_xts = std::min( bid_quantity_xts, ask_quantity_xts );

                   if( ask_quantity_xts == quantity_xts )
                   {
                      mtrx.ask_received = current_ask_balance;
                      mtrx.bid_paid     = current_ask_balance;
                      xts_paid_by_short = quantity_xts;
                   }
                   else
                   {
                      mtrx.ask_received   = quantity_xts * mtrx.ask_price;
                      mtrx.bid_paid       = current_bid_balance * mtrx.bid_price;
                      xts_paid_by_short   = current_bid_balance;
                   }

                   // rounding errors go into collateral, round to the nearest 1 XTS
                   if( bid_quantity_xts.amount - quantity_xts.amount < BTS_BLOCKCHAIN_PRECISION )
                      xts_paid_by_short = bid_quantity_xts;

                   mtrx.ask_paid       = quantity_xts;
                   mtrx.bid_received   = quantity_xts;

                   // the short always pays the quantity.

                   FC_ASSERT( xts_paid_by_short <= current_bid_balance );

                   if( mtrx.ask_paid.amount > *_current_ask->collateral )
                   {
                      wlog( "skipping margin call because best bid is insufficient to cover" );
                      // skip it... 
                      _current_ask.reset();
                      continue;
                   }

                   if( mtrx.bid_price < min_cover_ask )
                   {
                      wlog( "skipping short price ${x} < min_cover_ask ${b}", ("x",_current_bid->get_price())("b", min_cover_ask)  );
                      _current_ask.reset();
                      continue;
                   }

                   pay_current_short( mtrx, xts_paid_by_short, *quote_asset );
                   pay_current_cover( mtrx, *quote_asset );

                   market_stat->bid_depth -= xts_paid_by_short.amount;
                   market_stat->ask_depth += xts_paid_by_short.amount;
                   market_stat->ask_depth -= mtrx.ask_paid.amount;
                }
                else if( _current_ask->type == cover_order && _current_bid->type == bid_order )
                {
                   elog( "CURRENT ASK IS COVER" );
                   FC_ASSERT( quote_asset->is_market_issued() && base_id == 0 );
                   if( mtrx.ask_price < mtrx.bid_price ) 
                      break; // the call price has not been reached

                   mtrx.ask_price = mtrx.bid_price;
                   auto usd_exchanged = std::min( current_bid_balance, current_ask_balance );
                  
                   // TODO: verify that ask_price is within the valid range (median feed)

                   mtrx.bid_paid     = usd_exchanged;
                   mtrx.ask_received = usd_exchanged;
                   mtrx.ask_paid     = usd_exchanged * mtrx.bid_price;
                   mtrx.bid_received = mtrx.ask_paid;

                   /**
                    *  Don't cover at prices below the minimum cover price this is designed to prevent manipulation
                    *  where the cover must accept very low USD valuations 
                    */
                   if( mtrx.bid_price < min_cover_ask )
                   {
                      wlog( "skipping ${x} < min_cover_ask ${b}", ("x",_current_bid->get_price())("b", min_cover_ask)  );
                      _current_ask.reset();
                      continue;
                   }

                   /** In the event that there is insufficient collateral, the networks version of
                    * FDIC insurance kicks in to buy back BitUSD.  This helps keep BitUSD holders
                    * whole at the expense of XTS holders who are debased.  This should only happen
                    * when the highest bid is at a price that consumes all collateral.
                    */
                   if( mtrx.ask_paid.amount > *_current_ask->collateral )
                   {
                      market_stat->ask_depth -= *_current_ask->collateral;
                      auto fdic_insurance = mtrx.ask_paid.amount - *_current_ask->collateral;
                      *_current_ask->collateral += fdic_insurance;
                      base_asset->current_share_supply += fdic_insurance;
                   }
                   else
                   {
                      market_stat->ask_depth -= mtrx.ask_paid.amount;
                   }
                   pay_current_bid( mtrx, *quote_asset );
                   pay_current_cover( mtrx, *quote_asset );
                }
                else if( _current_ask->type == ask_order && _current_bid->type == short_order )
                {
                   if( mtrx.bid_price < mtrx.ask_price ) break;
                   FC_ASSERT( quote_asset->is_market_issued() && base_id == 0 );
                   auto quantity_xts   = std::min( bid_quantity_xts, ask_quantity_xts );

                   mtrx.bid_paid       = quantity_xts * mtrx.bid_price;
                   mtrx.ask_paid       = quantity_xts;
                   mtrx.bid_received   = mtrx.ask_paid;
                   mtrx.ask_received   = mtrx.ask_paid * mtrx.ask_price;

                   //ulog( "bid_quat: ${b}  balance ${q}  ask ${a}\n", ("b",quantity_xts)("q",*_current_bid)("a",*_current_ask) );
                   xts_paid_by_short   = quantity_xts; //bid_quantity_xts;

                   // rounding errors go into collateral, round to the nearest 1 XTS
                   if( bid_quantity_xts.amount - quantity_xts.amount < BTS_BLOCKCHAIN_PRECISION )
                      xts_paid_by_short = bid_quantity_xts;

                   if( mtrx.bid_price > max_short_bid )
                   {
                      wlog( "skipping short ${x} < max_short_bid ${b}", ("x",mtrx.bid_price)("b", max_short_bid)  );
                      _current_bid.reset();
                      continue;
                   }

                   FC_ASSERT( xts_paid_by_short <= _current_bid->get_balance() );
                   pay_current_short( mtrx, xts_paid_by_short, *quote_asset );
                   pay_current_ask( mtrx, *base_asset );

                   market_stat->bid_depth -= xts_paid_by_short.amount;
                   market_stat->ask_depth += xts_paid_by_short.amount;

                   mtrx.fees_collected = mtrx.bid_paid - mtrx.ask_received;
                }
                else if( _current_ask->type == ask_order && _current_bid->type == bid_order )
                {
                   if( mtrx.bid_price < mtrx.ask_price ) break;
                   auto quantity_xts = std::min( bid_quantity_xts, ask_quantity_xts );

                   mtrx.bid_paid       = quantity_xts * mtrx.bid_price;
                   // ask gets exactly what they asked for
                   mtrx.ask_received   = quantity_xts * mtrx.ask_price;
                   mtrx.ask_paid       = quantity_xts;
                   mtrx.bid_received   = quantity_xts;

                   // because there could be rounding errors, we assume that if we are 
                   // filling the bid quantity we are paying the full balance rather
                   // than suffer rounding errors.
                   if( quantity_xts == bid_quantity_xts )
                   {
                      mtrx.bid_paid = current_bid_balance;
                   }
                   pay_current_bid( mtrx, *quote_asset );
                   pay_current_ask( mtrx, *base_asset );

                   market_stat->ask_depth -= mtrx.ask_paid.amount;
                   mtrx.fees_collected = mtrx.bid_paid - mtrx.ask_received;
                }


                push_market_transaction(mtrx);
                accumulate_fees( mtrx, *quote_asset );
             } // while( next bid && next ask )


             // update any fees collected
             _pending_state->store_asset_record( *quote_asset );
             _pending_state->store_asset_record( *base_asset );


             market_stat->last_error.reset();

             if( market_stat->avg_price_24h.ratio == fc::uint128_t() && median_price )
             {
                market_stat->avg_price_24h = *median_price;
             }
             else
             {
                if( _current_bid && _current_ask )
                {
                   // after the market is running solid we can use this metric...
                   market_stat->avg_price_24h.ratio *= (BTS_BLOCKCHAIN_BLOCKS_PER_DAY-1);
                   market_stat->avg_price_24h.ratio += _current_bid->get_price().ratio;
                   market_stat->avg_price_24h.ratio += _current_ask->get_price().ratio;
                   market_stat->avg_price_24h.ratio /= (BTS_BLOCKCHAIN_BLOCKS_PER_DAY+1);
                }
             
                if( quote_asset->is_market_issued() )
                {
                   if( market_stat->ask_depth < BTS_BLOCKCHAIN_MARKET_DEPTH_REQUIREMENT ||
                       market_stat->bid_depth < BTS_BLOCKCHAIN_MARKET_DEPTH_REQUIREMENT 
                     )
                   {
                     std::string reason = "After executing orders there was insufficient depth remaining";
                     FC_CAPTURE_AND_THROW( insufficient_depth, (reason)(market_stat)(BTS_BLOCKCHAIN_MARKET_DEPTH_REQUIREMENT) );
                   }
                }
             }
             _pending_state->store_market_status( *market_stat );

             wlog( "done matching orders" );
             _pending_state->apply_changes();
        } 
        catch( const fc::exception& e )
        {
           wlog( "error executing market ${quote} / ${base}\n ${e}", ("quote",quote_id)("base",base_id)("e",e.to_detail_string()) );
           auto market_state = _prior_state->get_market_status( quote_id, base_id );
           if( !market_state )
              market_state = market_status( quote_id, base_id, 0, 0 );
           market_state->last_error = e;
           _prior_state->store_market_status( *market_state );
        }
      } // execute(...)
      void push_market_transaction( const market_transaction& mtrx )
      { try {
          FC_ASSERT( mtrx.bid_paid.amount >= 0 );
          FC_ASSERT( mtrx.ask_paid.amount >= 0 );
          FC_ASSERT( mtrx.bid_received.amount >= 0 );
          FC_ASSERT( mtrx.ask_received .amount>= 0 );
          FC_ASSERT( mtrx.bid_paid >= mtrx.ask_received );
          FC_ASSERT( mtrx.ask_paid >= mtrx.bid_received );
          FC_ASSERT( mtrx.fees_collected.amount >= 0 );

          //elog( "${trx}", ("trx", fc::json::to_pretty_string( mtrx ) ) );

          _market_transactions.push_back(mtrx);
      } FC_CAPTURE_AND_RETHROW( (mtrx) ) }

      void pay_current_short(const market_transaction& mtrx, const asset& xts_paid_by_short, asset_record& quote_asset  ) 
      { try {
          FC_ASSERT( _current_bid->type == short_order );
          FC_ASSERT( mtrx.bid_type == short_order );

          FC_ASSERT( mtrx.ask_paid == xts_paid_by_short, "", ("mtrx",mtrx)("xts_paid_by_short",xts_paid_by_short) );

          quote_asset.current_share_supply += mtrx.bid_paid.amount;

          auto collateral  = xts_paid_by_short + xts_paid_by_short;
          if( mtrx.bid_paid.amount <= 0 )
          {
             //ulog( "bid paid ${c}  collateral ${xts} \nbid: ${current}\nask: ${ask}", ("c",mtrx.bid_paid)("xts",xts_paid_by_short)("current", (*_current_bid))("ask",*_current_ask) );
             _current_bid->state.balance -= xts_paid_by_short.amount;
             return;
          }

          auto cover_price = mtrx.bid_paid / asset( (3*collateral.amount)/4, _base_id );

          market_index_key cover_index( cover_price, _current_bid->get_owner() );
          auto ocover_record = _pending_state->get_collateral_record( cover_index );

          if( NOT ocover_record ) ocover_record = collateral_record();

          ocover_record->collateral_balance += collateral.amount;
          ocover_record->payoff_balance += mtrx.bid_paid.amount;

          FC_ASSERT( ocover_record->payoff_balance >= 0, "", ("record",ocover_record) );
          FC_ASSERT( ocover_record->collateral_balance >= 0 , "", ("record",ocover_record));

          _current_bid->state.balance -= xts_paid_by_short.amount;
          FC_ASSERT( _current_bid->state.balance >= 0 );

          _pending_state->store_collateral_record( cover_index, *ocover_record );

          _pending_state->store_short_record( _current_bid->market_index, _current_bid->state );
      } FC_CAPTURE_AND_RETHROW( (mtrx)  ) }

      void pay_current_bid( const market_transaction& mtrx, asset_record& quote_asset )
      { try {
          FC_ASSERT( _current_bid->type == bid_order );
          FC_ASSERT( mtrx.bid_type == bid_order );
          _current_bid->state.balance -= mtrx.bid_paid.amount; 
          FC_ASSERT( _current_bid->state.balance >= 0 );

          auto bid_payout = _pending_state->get_balance_record( 
                                    withdraw_condition( withdraw_with_signature(mtrx.bid_owner), _base_id ).get_address() );
          if( !bid_payout )
             bid_payout = balance_record( mtrx.bid_owner, asset(0,_base_id), 0 );

          bid_payout->balance += mtrx.bid_received.amount;
          bid_payout->last_update = _pending_state->now();
          _pending_state->store_balance_record( *bid_payout );


          // if the balance is less than 1 XTS then it gets collected as fees.
          if( (_current_bid->get_quote_quantity() * _current_bid->get_price()).amount == 0 )
          {
              quote_asset.collected_fees += _current_bid->get_quote_quantity().amount;
              _current_bid->state.balance = 0;
          }
          _pending_state->store_bid_record( _current_bid->market_index, _current_bid->state );
      } FC_CAPTURE_AND_RETHROW( (mtrx) ) }

      void pay_current_cover( market_transaction& mtrx, asset_record& quote_asset )
      { try {
          FC_ASSERT( _current_ask->type == cover_order );
          FC_ASSERT( mtrx.ask_type == cover_order );

          // we are in the margin call range... 
          _current_ask->state.balance  -= mtrx.bid_paid.amount;
          *(_current_ask->collateral)  -= mtrx.ask_paid.amount; 

          quote_asset.current_share_supply -= mtrx.ask_received.amount;

          FC_ASSERT( _current_ask->state.balance >= 0 );
          FC_ASSERT( *_current_ask->collateral >= 0, "", ("mtrx",mtrx)("_current_ask", _current_ask)  );

          if( _current_ask->state.balance == 0 ) // no more USD left
          { // send collateral home to mommy & daddy
                wlog( "            collateral balance is now 0!" ); 
                auto ask_balance_address = withdraw_condition( 
                                                  withdraw_with_signature(_current_ask->get_owner()), 
                                                  _base_id ).get_address();

                auto ask_payout = _pending_state->get_balance_record( ask_balance_address );
                if( !ask_payout )
                   ask_payout = balance_record( _current_ask->get_owner(), asset(0,_base_id), 0 );

                auto left_over_collateral = (*_current_ask->collateral);

                /** charge 5% fee for having a margin call */
                auto fee = (left_over_collateral * 5000 )/100000;
                left_over_collateral -= fee;
                // when executing a cover order, it always takes the exact price of the
                // highest bid, so there should be no fees paid *except* this.
                FC_ASSERT( mtrx.fees_collected.amount == 0 );
               // TODO: these go to the network... as dividends..
                mtrx.fees_collected  += asset(fee,0);

                auto prev_accumulated_fees = _pending_state->get_accumulated_fees();
                _pending_state->set_accumulated_fees( prev_accumulated_fees + fee );

                ask_payout->balance += left_over_collateral;
                ask_payout->last_update = _pending_state->now();

                _pending_state->store_balance_record( *ask_payout );
                _current_ask->collateral = 0;
          }
          //ulog( "storing collateral ${c}", ("c",_current_ask) );

          // the collateral position is now worse than before, if we don't update the market index then
          // the index price will be "wrong"... ie: the call price should move up based upon the fact
          // that we consumed more collateral than USD...  
          //
          // If we leave it as is, then chances are we will end up covering the entire amount this time, 
          // but we cannot use the price on the call for anything other than a trigger.  
          _pending_state->store_collateral_record( _current_ask->market_index, 
                                                   collateral_record( *_current_ask->collateral, 
                                                                      _current_ask->state.balance ) );
      } FC_CAPTURE_AND_RETHROW( (mtrx) ) }

      void pay_current_ask( const market_transaction& mtrx, asset_record& base_asset )
      { try {
          if( _current_ask->type == ask_order ) // update ask + payout
          {
             _current_ask->state.balance -= mtrx.ask_paid.amount; 
             FC_ASSERT( _current_ask->state.balance >= 0 );
 
             auto ask_balance_address = withdraw_condition( withdraw_with_signature(mtrx.ask_owner), _quote_id ).get_address();
             auto ask_payout = _pending_state->get_balance_record( ask_balance_address );
             if( !ask_payout )
                ask_payout = balance_record( mtrx.ask_owner, asset(0,_quote_id), 0 );
             ask_payout->balance += mtrx.ask_received.amount; 
             ask_payout->last_update = _pending_state->now();
 
             _pending_state->store_balance_record( *ask_payout );


             // if the balance is less than 1 XTS then it gets collected as fees.
             if( (_current_ask->get_quantity() * _current_ask->get_price()).amount == 0 )
             {
                 base_asset.collected_fees += _current_ask->get_quantity().amount;
                 _current_ask->state.balance = 0;
             }
             _pending_state->store_ask_record( _current_ask->market_index, _current_ask->state );

          } else { // if cover_order
          }
      } FC_CAPTURE_AND_RETHROW( (mtrx) )  } // pay_current_ask

      void accumulate_fees( const market_transaction& mtrx, asset_record& quote_asset )
      {
         if( mtrx.fees_collected.amount == 0 ) return;
         if( mtrx.fees_collected.asset_id == 0 )
         {
             auto prev_accumulated_fees = _pending_state->get_accumulated_fees();
             _pending_state->set_accumulated_fees( prev_accumulated_fees + mtrx.fees_collected.amount );
         }
         else
         {
            FC_ASSERT( quote_asset.id == mtrx.fees_collected.asset_id );
            quote_asset.collected_fees += mtrx.fees_collected.amount; 
         }
      }

      bool get_next_bid()
      { try {
         if( _current_bid && _current_bid->get_quantity().amount > 0 ) 
            return _current_bid.valid();

         _current_bid.reset();
         if( _bid_itr.valid() )
         {
            auto bid = market_order( bid_order, _bid_itr.key(), _bid_itr.value() );
            if( bid.get_price().quote_asset_id == _quote_id && 
                bid.get_price().base_asset_id == _base_id )
            {
                _current_bid = bid;
            }
         }

         if( _short_itr.valid() )
         {
            auto bid = market_order( short_order, _short_itr.key(), _short_itr.value() );
            //wlog( "SHORT ITER VALID: ${o}", ("o",bid) );
            if( bid.get_price().quote_asset_id == _quote_id && 
                bid.get_price().base_asset_id == _base_id )
            {
                if( !_current_bid || _current_bid->get_price() < bid.get_price() )
                {
                   --_short_itr;
                   _current_bid = bid;
                   return _current_bid.valid();
                }
            }
         }
         else
         {
            // wlog( "           No Shorts         ****   " );
         }
         if( _bid_itr.valid() ) --_bid_itr;
         return _current_bid.valid();
      } FC_CAPTURE_AND_RETHROW() }

      bool get_next_ask()
      { try {
         if( _current_ask && _current_ask->state.balance > 0 )
         {
            idump( (_current_ask) );
            return _current_ask.valid();
         }
         _current_ask.reset();

         /**
          *  Margin calls take priority over all other ask orders
          */
         while( _current_bid && _collateral_itr.valid() )
         {
            auto cover_ask = market_order( cover_order,
                                     _collateral_itr.key(), 
                                     order_record(_collateral_itr.value().payoff_balance), 
                                     _collateral_itr.value().collateral_balance  );

            if( cover_ask.get_price().quote_asset_id == _quote_id &&
                cover_ask.get_price().base_asset_id == _base_id )
            {
#if 0 // we will cover at any price that is within range of the price feed.
                if( _current_bid->get_price() < cover_ask.get_highest_cover_price()  )
                {
                   // cover position has been blown out, current bid is not able to
                   // cover the position, so it will sit until the price recovers 
                   // enough to fill it.  
                   //
                   // The idea here is that the longs have agreed to a maximum 
                   // protection equal to the collateral.  If they would like to
                   // sell their USD for XTS this is the best price the short is
                   // obligated to offer.  
                   //
                   // In other words, this ask MUST be filled before any thing else
                   FC_CAPTURE_AND_THROW( insufficient_collateral, (_current_bid)(cover_ask)(cover_ask.get_highest_cover_price()));
                   --_collateral_itr;
                   continue;
                }
#endif
                // max bid must be greater than call price
                if( _current_bid->get_price() < cover_ask.get_price() )
                {
                 //  if( _current_ask->get_price() > cover_ask.get_price() )
                   {
                      _current_ask = cover_ask;
                      _current_payoff_balance = _collateral_itr.value().payoff_balance;
                      wlog( "--collateral_iter" );
                      --_collateral_itr;
                      idump( (_current_ask) );
                      return _current_ask.valid();
                   }
                }
            }
            break;
         }

         if( _ask_itr.valid() )
         {
            auto ask = market_order( ask_order, _ask_itr.key(), _ask_itr.value() );
            wlog( "ASK ITER VALID: ${o}", ("o",ask) );
            if( ask.get_price().quote_asset_id == _quote_id && 
                ask.get_price().base_asset_id == _base_id )
            {
                _current_ask = ask;
            }
            ++_ask_itr;
            idump( (_current_ask) );
            return true;
         }
         return _current_ask.valid();
      } FC_CAPTURE_AND_RETHROW() }

      pending_chain_state_ptr     _pending_state;
      pending_chain_state_ptr     _prior_state;
      chain_database_impl&        _db_impl;
      
      optional<market_order>      _current_bid;
      optional<market_order>      _current_ask;
      share_type                  _current_payoff_balance;
      asset_id_type               _quote_id;
      asset_id_type               _base_id;

      vector<market_transaction>  _market_transactions;

      bts::db::level_map< market_index_key, order_record >::iterator       _bid_itr;
      bts::db::level_map< market_index_key, order_record >::iterator       _ask_itr;
      bts::db::level_map< market_index_key, order_record >::iterator       _short_itr;
      bts::db::level_map< market_index_key, collateral_record >::iterator  _collateral_itr;
};

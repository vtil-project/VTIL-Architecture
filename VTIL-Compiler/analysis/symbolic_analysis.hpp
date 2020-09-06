#pragma once
#include <list>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <vtil/arch>
#include <vtil/symex>
#include <vtil/common>

namespace vtil::analysis
{
	// TODO: 
	// - sp_offset based r/w discarding
	// - cross segment read / write
	// - should not be an analysis, but rather a seperate representation of basic_block.
	//
	struct symbolic_segment : vm_interface
	{
		// Segment limits and the reason of exit.
		//
		vm_exit_reason exit_reason = vm_exit_reason::none;
		il_const_iterator segment_begin;
		il_const_iterator segment_end = {};

		// Instructions that could not translated into symbolic equivalents,
		// that should be appended following this segment.
		//
		std::vector<il_const_iterator> suffix;

		// If last segment, branch details.
		//
		bool is_branch_real = false;
		bool is_branch_exiting = false;
		symbolic::expression::reference branch_cc = nullptr;
		std::vector<symbolic::expression::reference> branch_targets;

		// State of the virtual machine.
		//
		bool is_executing = false;
		symbolic::memory memory_state;
		symbolic::context register_state;

		// Reference tracking.
		//
		std::unordered_map<register_desc::weak_id, uint64_t> register_references;
		std::unordered_map<symbolic::pointer, uint64_t> memory_references;

		// Basic construction from iterator.
		//
		symbolic_segment( il_const_iterator it ) : segment_begin( std::move( it ) ) {}

		// Implement VM interface.
		//
		symbolic::expression::reference read_register( const register_desc& desc ) const override
		{
			// Try reading from register state.
			//
			uint64_t known = 0, read = desc.get_mask();
			auto result = register_state.read( desc, segment_begin, &known );

			// If we were executing and value had unknowns, add to references, return the result.
			//
			if ( is_executing && ( read & ~known ) )
				make_mutable( register_references )[ desc ] |= read & ~known;
			return result;
		}
		symbolic::expression::reference read_memory( const symbolic::expression::reference& pointer, size_t byte_count ) const override
		{
			// Parse the pointer.
			//
			bitcnt_t size = math::narrow_cast<bitcnt_t>( byte_count * 8 );
			symbolic::pointer spointer = pointer;
			
			// Try reading from memory state.
			//
			uint64_t known = 0, read = math::fill( size );
			auto result = memory_state.read( spointer, size, segment_begin, &known );

			// If we were executing and value had unknowns, add to references, return the result.
			//
			if ( is_executing && ( read & ~known ) )
				make_mutable( memory_references )[ spointer ] |= read & ~known;
			return result;
		}
		void write_register( const register_desc& desc, symbolic::expression::reference value ) override
		{
			// Make value lazy, write to register state.
			//
			register_state.write( desc, std::move( value.make_lazy() ) );
		}
		bool write_memory( const symbolic::expression::reference& pointer, deferred_value<symbolic::expression::reference> value, bitcnt_t size ) override
		{
			// Created deferred result proxy making value lazy, write to memory state.
			//
			deferred_result value_n = [ & ]() -> auto { return std::move( value.get().make_lazy() ); };
			return memory_state.write( pointer, value_n, size ).has_value();
		}
		vm_exit_reason execute( const instruction& ins ) override
		{
			// Set is_executing flag during execution.
			//
			finally _g( [ & ] () { is_executing = false; } );
			is_executing = true;

			// Handle branching instructions.
			//
			if ( ins.base->is_branching() )
			{
				auto cvt_operand = [ & ] ( int i ) -> symbolic::expression::reference
				{
					const operand& op = ins.operands[ i ];

					// If operand is a register:
					//
					if ( op.is_register() )
					{
						// Trace the source register.
						//
						symbolic::expression::reference result = read_register( op.reg() );

						// If stack pointer, add the current virtual offset.
						//
						if ( op.reg().is_stack_pointer() )
							result = result + ins.sp_offset;

						// Return the result.
						//
						return result;
					}
					// If it is an immediate, convert into constant expression and return.
					//
					else
					{
						fassert( op.is_immediate() );
						return { op.imm().i64, op.imm().bit_count };
					}
				};

				// If real branch:
				//
				if ( ins.base == &ins::vexit ||
					 ins.base == &ins::vxcall )
				{
					branch_targets.emplace_back( cvt_operand( 0 ) );
					is_branch_real = true;
					is_branch_exiting = ins.base == &ins::vexit;
					branch_cc = nullptr;
					return vm_exit_reason::stream_end;
				}
				// If unconditional jump:
				//
				else if ( ins.base == &ins::jmp )
				{
					branch_targets.emplace_back( cvt_operand( 0 ) );
					is_branch_real = false;
					branch_cc = nullptr;
					return vm_exit_reason::stream_end;
				}
				// If conditional jump:
				//
				else if ( ins.base == &ins::js )
				{
					branch_targets.emplace_back( cvt_operand( 1 ) );
					branch_targets.emplace_back( cvt_operand( 2 ) );
					is_branch_real = false;
					branch_cc = cvt_operand( 0 );
					return vm_exit_reason::stream_end;
				}
				unreachable();
			}

			// Halt if instruction is volatile.
			//
			if ( ins.is_volatile() )
				return vm_exit_reason::unknown_instruction;

			// Halt if instruction accesses volatile registers excluding ?UD.
			//
			for ( auto& op : ins.operands )
				if ( op.is_register() && op.reg().is_volatile() && !op.reg().is_undefined() )
					return vm_exit_reason::unknown_instruction;

			// Invoke original handler.
			//
			return vm_interface::execute( ins );
		}
	};

	struct symbolic_analysis : synchronized_context<basic_block>
	{
		// List of segments, ideally just one if none of it quit due to alias analysis failure.
		//
		std::list<symbolic_segment> segments;

		// Wrap around std::list.
		//
		auto begin() { return segments.begin(); }
		auto end() { return segments.end(); }
		auto begin() const { return segments.cbegin(); }
		auto end() const { return segments.cend(); }
		auto size() const { return segments.size(); }

		// Updates the symbolic analysis.
		//
		void update( const basic_block* block ) override
		{
			// Reset all segments.
			//
			segments.clear();

			// Until we reach the end of the block:
			//
			for ( il_const_iterator it = block->begin(); !it.is_end(); )
			{
				// Create a new segment and run the VM.
				//
				symbolic_segment* seg = &segments.emplace_back( it );
				std::tie( it, seg->exit_reason ) = seg->run( it );
				seg->segment_end = it;

				// If end, break.
				//
				if ( seg->exit_reason == vm_exit_reason::stream_end )
					break;

				// If not alias failure:
				//
				if ( seg->exit_reason != vm_exit_reason::alias_failure )
				{
					// If VM state is empty, and this is not the first segment, pop it.
					//
					if ( seg->memory_state.size() == 0 &&
						 seg->register_state.size() == 0 &&
						 segments.size() > 1 )
					{
						segments.pop_back();
						seg = &segments.back();
					}

					// Append the instruction to the suffix.
					//
					seg->suffix.emplace_back( it++ );
					seg->segment_end = it;
				}
			}
		}

		// Pre-simplifies all current expressions stored.
		//
		void prepare( bool pack = true )
		{
			// For each segment:
			//
			for ( auto& seg : segments )
			{
				// Simplify each partial register value:
				//
				for ( auto& [k, v] : seg.register_state )
				{
					math::bit_enum( v.bitmap, [ &, v = std::ref( v.linear_store ) ]( bitcnt_t i )
					{
						v[ i ].simplify( pack );
					} );
				}

				// Simplify each memory value:
				//
				for ( auto& [k, v] : seg.memory_state )
					v.simplify( pack );

				// Simplify the branch:
				//
				if ( seg.branch_targets.size() )
				{
					for ( auto& v : seg.branch_targets )
						v.simplify( true );
					if ( auto& v = seg.branch_cc )
						v.simplify( true );

					// If non-const JMP, try converting into JS.
					//
					if ( !seg.branch_cc && seg.branch_targets[ 0 ]->depth > 2 )
					{
						// Enumerate into the branch target:
						//
						auto& statement = seg.branch_targets[ 0 ];
						std::function<void( const symbolic::expression& )> ccscan = [ & ] ( const symbolic::expression& ccexp )
						{
							// If we've already found a condition, skip traversal.
							//
							if ( seg.branch_cc )
								return;

							// If pointer, traverse into it.
							//
							if ( ccexp.is_variable() )
							{
								auto& var = ccexp.uid.get<symbolic::variable>();
								if ( var.is_memory() )
									var.mem().decay()->enumerate( ccscan );
							}

							// If this is a possible condition:
							//
							if ( ( ccexp.value.unknown_mask() | ccexp.value.known_one() ) == 1 )
							{
								// Save the hash of default value.
								//
								auto hash_unchanged = statement->hash();

								// Calculate the approximation for CC:
								//
								auto exp_approx = ccexp.approximate();

								// Reverse the condition and calculate approximation for !CC:
								//
								auto inv_cc = ~ccexp;
								auto inv_approx = exp_approx;
								for ( auto& v : inv_approx.values )
									v ^= 1;

								// Declare the value of CC for current transformation and the transformer.
								//
								bool expected_value;
								std::function<void( symbolic::expression_delegate& )> cctfm = [ & ] ( symbolic::expression_delegate& pexp )
								{
									// If pointer, traverse into it.
									//
									if ( pexp->is_variable() )
									{
										auto& var = pexp->uid.get<symbolic::variable>();
										if ( var.is_memory() )
										{
											// If changed, replace pointer.
											//
											symbolic::expression::reference mexp = var.mem().decay();
											hash_t hash_0 = mexp.hash();
											mexp.transform( cctfm );
											if ( hash_0 != mexp.hash() )
												( +pexp )->uid = symbolic::variable{ var.at, { mexp, var.mem().bit_count } };
										}
									}

									// If possible condition:
									//
									if ( ( pexp->value.unknown_mask() | pexp->value.known_one() ) == 1 )
									{
										// If expected value or inverse, replace.
										//
										auto approx = pexp->approximate();
										if ( exp_approx == approx )
										{
											if ( pexp->equals( ccexp ) )
												*+pexp = { expected_value, 1 };
										}
										else if ( inv_approx == approx )
										{
											if ( pexp->equals( inv_cc ) )
												*+pexp = { !expected_value, 1 };
										}
									}
								};

								// Create two statements, one assuming CC=1, other assuming CC=0.
								//
								symbolic::expression::reference cnd_sat = statement;
								expected_value = true;
								cnd_sat.transform( cctfm );

								symbolic::expression::reference cnd_nsat = statement;
								expected_value = false;
								cnd_nsat.transform( cctfm );

								// If both expressions simplified, convert into JS branch.
								//
								if ( cnd_sat->hash() != hash_unchanged && cnd_nsat->hash() != hash_unchanged )
								{
									seg.branch_cc = ccexp;
									seg.branch_targets = { std::move( cnd_sat ), std::move( cnd_nsat ) };
								}
							}
						};
						statement->enumerate( ccscan );
					}
				}
			}
		}

		// Emits equivalent code into the given block.
		//
		void reemit( basic_block* block ) const
		{
			// Allocate a temporary block.
			//
			basic_block temporary_block = { block->owner, block->entry_vip };
			temporary_block.last_temporary_index = block->last_temporary_index;

			// For each segment:
			//
			std::vector<instruction> instruction_buffer;
			for ( auto it = segments.begin(); it != segments.end(); ++it )
			{
				auto& vm = *it;

				// Create a batch translator and reset instruction buffer.
				//
				batch_translator translator = { &temporary_block, vm.segment_begin };
				instruction_buffer.clear();

				// For each register state, skipping REG_SP:
				//
				for ( auto& pair : vm.register_state )
				{
					// Skip if value is unchanged or is stack pointer.
					//
					if ( !pair.second.bitmap || ( pair.first.flags & register_stack_pointer ) )
						continue;

					// Find out the highest bit modified and size we'd have to write.
					//
					bitcnt_t write_msb = math::msb( pair.second.bitmap );
					bitcnt_t write_size = pair.second.linear_store[ write_msb ].size();
					register_desc k = { pair.first, write_size, write_msb };

					// If partially inherited flags register with 4 or less changes:
					//
					if ( k.is_flags() && math::popcnt( pair.second.bitmap ) <= 4 )
					{
						// For each bit:
						//
						math::bit_enum( pair.second.bitmap, [ & ] ( bitcnt_t i )
						{
							// Read the value and pack.
							//
							auto v = symbolic::variable::pack_all( pair.second.linear_store[ i ] );

							// Buffer a mov instruction to the exact bit.
							//
							register_desc ks = k;
							ks.bit_offset += i;
							ks.bit_count = 1;
							instruction_buffer.emplace_back( &ins::mov, ks, translator << v );
						} );
						continue;
					}

					// Validate the register output.
					//
					fassert( !k.is_stack_pointer() && !k.is_read_only() );

					// Read the full register value and pack.
					//
					auto v = symbolic::variable::pack_all( vm.read_register( k ).simplify( true ) );

					// Buffer a mov instruction.
					//
					instruction_buffer.emplace_back( &ins::mov, k, translator << v );
				}

				// For each memory state:
				// -- TODO: Simplify memory state, merge if simplifies, discard if left as is.
				//
				for ( auto& [k, _v] : vm.memory_state )
				{
					// Pack registers and the expression.
					//
					auto v = symbolic::variable::pack_all( _v );

					// If pointer can be rewritten as $sp + C:
					//
					operand base, offset, value;
					if ( auto displacement = ( k - symbolic::CTX( vm.segment_begin )[ REG_SP ] ) )
					{
						// Buffer a str $sp, c, value.
						//
						instruction_buffer.emplace_back(
							&ins::str,
							REG_SP, make_imm<int64_t>( *displacement ), translator << v
						);
					}
					else
					{
						// Try to extract the offset from the compound expression.
						//
						int64_t offset = 0;
						auto exp = symbolic::variable::pack_all( k.base );
						if ( !exp->is_constant() )
						{
							using namespace symbolic::directive;

							std::vector<symbol_table_t> results;
							if ( fast_match( &results, A + U, exp ) )
							{
								exp = results.front().translate( A );
								offset = *results.front().translate( U )->get<int64_t>();
							}
							else if ( fast_match( &results, A - U, exp ) )
							{
								exp = results.front().translate( A );
								offset = -*results.front().translate( U )->get<int64_t>();
							}
						}

						// Translate the base address.
						//
						operand base = translator << exp;
						if ( base.is_immediate() )
						{
							operand tmp = temporary_block.tmp( base.bit_count() );
							instruction_buffer.emplace_back( &ins::mov, tmp, base );
							base = tmp;
						}

						// Buffer a str <ptr>, C, value.
						//
						instruction_buffer.emplace_back(
							&ins::str,
							base, make_imm( offset ), translator << v
						);
					}
				}

				// If we're branching, emit requirements into operands.
				//
				operand branch_cc;
				std::vector<operand> branch_targets;
				if ( vm.branch_targets.size() )
				{
					for ( auto& target : vm.branch_targets )
						branch_targets.emplace_back( translator << symbolic::variable::pack_all( target ) );
					if ( vm.branch_cc )
						branch_cc = translator << symbolic::variable::pack_all( vm.branch_cc );
				}

				// Emit entire buffer.
				//
				for ( auto& ins : instruction_buffer )
					temporary_block.emplace_back( std::move( ins ) );

				// Emit stack change if relevant.
				//
				int64_t sp_offset_d = 0;
				if ( auto sp_it = vm.register_state.value_map.find( REG_SP ); sp_it != vm.register_state.value_map.end() && sp_it->second.bitmap )
				{
					// If $SP + C:
					//
					auto new_sp = vm.read_register( REG_SP );
					if ( auto delta = ( new_sp - symbolic::CTX( vm.segment_begin )[ REG_SP ] ).get<int64_t>() )
					{
						temporary_block.shift_sp( sp_offset_d = *delta );
					}
					// Otherwise emit a { mov $sp, x }.
					//
					else
					{
						temporary_block.emplace_back( &ins::mov, REG_SP, translator << symbolic::variable::pack_all( new_sp ) );
					}
				}

				// Emit suffix.
				//
				if ( !vm.suffix.empty() )
				{
					int32_t sp_index_d = temporary_block.sp_index - vm.suffix.front()->sp_index;
					for ( auto& iit : vm.suffix )
					{
						instruction ins = *iit;
						ins.sp_index += sp_index_d;
						ins.sp_offset += sp_offset_d;
						if ( ins.base->reads_memory() &&
							 ins.memory_location().first.is_stack_pointer() )
							ins.memory_location().second += sp_offset_d;

						temporary_block.np_emplace_back( ins );
						temporary_block.sp_index = ins.sp_index;
						temporary_block.sp_offset = ins.sp_offset;
					}
				}

				// Adjust sp_offset.
				//
				temporary_block.sp_offset = std::prev( vm.segment_end )->sp_offset + sp_offset_d;

				// Emit branch.
				//
				if ( !branch_targets.empty() )
				{
					if ( vm.is_branch_real )
					{
						fassert( !vm.branch_cc && branch_targets.size() == 1 );

						if ( vm.is_branch_exiting )
							temporary_block.vexit( branch_targets[ 0 ] );
						else
							temporary_block.vxcall( branch_targets[ 0 ] );
					}
					else if ( vm.branch_cc )
					{
						fassert( branch_targets.size() == 2 );

						if ( branch_cc.is_register() )
							temporary_block.js( branch_cc, branch_targets[ 0 ], branch_targets[ 1 ] );
						else
							temporary_block.jmp( branch_cc.imm().u64 ? branch_targets[ 1 ] : branch_targets[ 0 ] );
					}
					else
					{
						fassert( branch_targets.size() == 1 );

						temporary_block.jmp( branch_targets[ 0 ] );
					}

					// Adjust end-of-block sp_offset.
					//
					temporary_block.sp_offset = vm.segment_end.block->sp_offset + sp_offset_d;
				}
			}

			// TODO: This is dumb but is due to symbolic variable stuff.
			//
			if ( block == segments.front().segment_begin.block )
				mark_dirty();

			// Copy temporary block over input.
			//
			block->assign( std::move( temporary_block ) );
			block->sp_index = temporary_block.sp_index;
			block->sp_offset = temporary_block.sp_offset;
			block->last_temporary_index = temporary_block.last_temporary_index;
		}

		// Dumps the current state.
		//
		void dump() const
		{
			using namespace logger;

			for ( auto it = segments.begin(); it != segments.end(); it++ )
			{
				auto& seg = *it;

				log<CON_GRN>( "[%s - %s]\n", seg.segment_begin, seg.segment_end );

				log<CON_CYN>( "- # Memory Ops:   %d\n", seg.memory_state.size() );
				log<CON_CYN>( "- # Register Ops: %d\n", seg.register_state.size() );
				log<CON_YLW>( "- Stack pointer:  %s\n", seg.register_state.read( REG_SP ) );

				for ( auto&& [m, v] : seg.memory_state )
					log( "%s => %s\n", m, v );
				log( "Ref: MEM = %s\n", seg.memory_references );
				log( "     REG = %s\n", seg.register_references );

				switch ( seg.exit_reason )
				{
					case vm_exit_reason::stream_end:
						log<CON_BLU>( "Exit due to stream end\n" );

						if ( seg.is_branch_real )
						{
							if ( seg.segment_begin.block->next.empty() )
								log<CON_RED>( "Real Exit     " );
							else
								log<CON_RED>( "Real Call     " );
						}
						else                      log<CON_BLU>( "Virtual Branch" );
						log<CON_BRG>( " => " );
						if ( seg.branch_cc )
						{
							log<CON_YLW>( "%s", seg.branch_cc );            log<CON_BRG>( " ? " );
							log<CON_GRN>( "%s", seg.branch_targets[ 0 ] );  log<CON_BRG>( " : " );
							log<CON_RED>( "%s\n", seg.branch_targets[ 1 ] );
						}
						else
						{
							log<CON_PRP>( "%s\n", seg.branch_targets );
						}

						break;
					case vm_exit_reason::alias_failure:
						log<CON_RED>( "Exit due to alias analysis failure @" );
						log<CON_BRG>( " \"%s\"\n", std::next( it )->segment_begin->to_string() );

						break;
					case vm_exit_reason::high_arithmetic:
						log<CON_RED>( "Exit due to high arithmetic:\n" );
						break;
					case vm_exit_reason::unknown_instruction:
						log<CON_PRP>( "Exit due to non-symbolic instruction:\n" );
						break;
				}
				for ( auto& ins : seg.suffix )
					log<CON_YLW>( " + %s\n", ins );
			}
		};
	};
};
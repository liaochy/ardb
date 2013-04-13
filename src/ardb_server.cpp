/*
 * ardb_server.cpp
 *
 *  Created on: 2013-4-8
 *      Author: wqy
 */
#include "ardb_server.hpp"
#include <stdarg.h>
#ifdef __USE_KYOTOCABINET__
#include "engine/kyotocabinet_engine.hpp"
#else
#include "engine/leveldb_engine.hpp"
#endif

#define REDIS_REPLY_STRING 1
#define REDIS_REPLY_ARRAY 2
#define REDIS_REPLY_INTEGER 3
#define REDIS_REPLY_NIL 4
#define REDIS_REPLY_STATUS 5
#define REDIS_REPLY_ERROR 6

#define FILL_STR_REPLY(R, S) do{\
	R.type = REDIS_REPLY_STRING;\
	R.str = S;\
}while(0)

namespace ardb
{
	static inline void fill_error_reply(ArdbReply& reply, const char* fmt, ...)
	{
		va_list ap;
		va_start(ap, fmt);
		char buf[1024];
		vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
		va_end(ap);
		reply.type = REDIS_REPLY_ERROR;
		reply.str = buf;
	}

	static inline void fill_status_reply(ArdbReply& reply, const char* fmt, ...)
	{
		va_list ap;
		va_start(ap, fmt);
		char buf[1024];
		vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
		va_end(ap);
		reply.type = REDIS_REPLY_STATUS;
		reply.str = buf;
	}

	static inline void fill_int_reply(ArdbReply& reply, int64 v)
	{
		reply.type = REDIS_REPLY_INTEGER;
		reply.integer = v;
	}
	static inline void fill_array_reply(ArdbReply& reply, ValueArray& v)
	{
		reply.type = REDIS_REPLY_ARRAY;
		ValueArray::iterator it = v.begin();
		while (it != v.end())
		{
			ValueObject& vo = *it;
			ArdbReply r;
			if (vo.type == EMPTY)
			{
				r.type = REDIS_REPLY_NIL;
			}
			else
			{
				FILL_STR_REPLY(r, vo.ToString());
			}
			reply.elements.push_back(r);
			it++;
		}
	}

	static void encode_reply(Buffer& buf, ArdbReply& reply)
	{
		switch (reply.type)
		{
			case REDIS_REPLY_NIL:
			{
				buf.Printf("$-1\r\n");
				break;
			}
			case REDIS_REPLY_STRING:
			{
				buf.Printf("$%d\r\n", reply.str.size());
				if (reply.str.size() > 0)
				{
					buf.Printf("%s\r\n", reply.str.c_str());
				}
				else
				{
					buf.Printf("\r\n");
				}
				break;
			}
			case REDIS_REPLY_ERROR:
			{
				buf.Printf("-%s\r\n", reply.str.c_str());
				break;
			}
			case REDIS_REPLY_INTEGER:
			{
				buf.Printf(":%lld\r\n", reply.integer);
				break;
			}
			case REDIS_REPLY_ARRAY:
			{
				buf.Printf("*%d\r\n", reply.elements.size());
				size_t i = 0;
				while (i < reply.elements.size())
				{
					encode_reply(buf, reply.elements[i]);
					i++;
				}
				break;
			}
			case REDIS_REPLY_STATUS:
			{
				buf.Printf("+%s\r\n", reply.str.c_str());
				break;
			}
			default:
			{
				ERROR_LOG("Recv unexpected redis reply type:%d", reply.type);
				break;
			}
		}
	}

	int ArdbServer::ParseConfig(const Properties& props, ArdbServerConfig& cfg)
	{
		conf_get_int64(props, "port", cfg.listen_port);
		conf_get_string(props, "bind", cfg.listen_host);
		conf_get_string(props, "unixsocket", cfg.listen_unix_path);
		std::string daemonize;
		conf_get_string(props, "daemonize", daemonize);
		daemonize = string_tolower(daemonize);
		if (daemonize == "yes")
		{
			cfg.daemonize = true;
		}
		return 0;
	}

	ArdbServer::ArdbServer() :
			m_service(NULL), m_db(NULL), m_engine(NULL)
	{
		struct RedisCommandHandlerSetting settingTable[] = { { "ping",
		        &ArdbServer::Ping, 0, 0 }, { "echo", &ArdbServer::Echo, 1, 1 },
		        { "quit", &ArdbServer::Quit, 0, 0 }, { "shutdown",
		                &ArdbServer::Shutdown, 0, 1 }, { "slaveof",
		                &ArdbServer::Slaveof, 2, 2 }, { "select",
		                &ArdbServer::Select, 1, 1 }, { "append",
		                &ArdbServer::Append, 2, 2 }, { "get", &ArdbServer::Get,
		                1, 1 }, { "set", &ArdbServer::Set, 2, 7 }, { "del",
		                &ArdbServer::Del, 1, -1 }, { "exists",
		                &ArdbServer::Exists, 1, 1 }, { "expire",
		                &ArdbServer::Expire, 2, 2 }, { "expireat",
		                &ArdbServer::Expireat, 2, 2 }, { "persist",
		                &ArdbServer::Persist, 1, 1 }, { "type",
		                &ArdbServer::Type, 1, 1 }, { "bitcount",
		                &ArdbServer::Bitcount, 1, 3 }, { "bitop",
		                &ArdbServer::Bitop, 3, -1 }, { "decr",
		                &ArdbServer::Decr, 1, 1 }, { "decrby",
		                &ArdbServer::Decrby, 2, 2 }, { "getbit",
		                &ArdbServer::GetBit, 2, 2 }, { "getrange",
		                &ArdbServer::GetRange, 3, 3 }, { "getset",
		                &ArdbServer::GetSet, 2, 2 }, { "incr",
		                &ArdbServer::Incr, 1, 1 }, { "incrby",
		                &ArdbServer::Incrby, 2, 2 }, { "incrbyfloat",
		                &ArdbServer::IncrbyFloat, 2, 2 }, { "mget",
		                &ArdbServer::MGet, 1, -1 }, { "mset", &ArdbServer::MSet,
		                2, -1 }, { "msetnx", &ArdbServer::MSetNX, 2, -1 }, {
		                "psetex", &ArdbServer::MSetNX, 3, 3 }, { "setbit",
		                &ArdbServer::SetBit, 3, 3 }, { "setex",
		                &ArdbServer::SetEX, 3, 3 }, { "setnx",
		                &ArdbServer::SetNX, 2, 2 }, { "setrange",
		                &ArdbServer::SetRange, 3, 3 }, { "strlen",
		                &ArdbServer::Strlen, 1, 1 } };

		uint32 arraylen = arraysize(settingTable);
		for (uint32 i = 0; i < arraylen; i++)
		{
			m_handler_table[settingTable[i].name] = settingTable[i];
		}
	}
	ArdbServer::~ArdbServer()
	{

	}

	int ArdbServer::Type(ArdbConnContext& ctx, ArgumentArray& cmd)
	{
		int ret = m_db->Type(ctx.currentDB, cmd[0]);
		switch (ret)
		{
			case SET_ELEMENT:
			{
				fill_status_reply(ctx.reply, "set");
				break;
			}
			case LIST_META:
			{
				fill_status_reply(ctx.reply, "list");
				break;
			}
			case ZSET_ELEMENT_SCORE:
			{
				fill_status_reply(ctx.reply, "zset");
				break;
			}
			case HASH_FIELD:
			{
				fill_status_reply(ctx.reply, "hash");
				break;
			}
			case KV:
			{
				fill_status_reply(ctx.reply, "string");
				break;
			}
			case TABLE_META:
			{
				fill_status_reply(ctx.reply, "table");
				break;
			}
			default:
			{
				fill_status_reply(ctx.reply, "none");
				break;
			}
		}
		return 0;
	}

	int ArdbServer::Persist(ArdbConnContext& ctx, ArgumentArray& cmd)
	{
		int ret = m_db->Persist(ctx.currentDB, cmd[0]);
		fill_int_reply(ctx.reply, ret == 0 ? 1 : 0);
		return 0;
	}

	int ArdbServer::Expire(ArdbConnContext& ctx, ArgumentArray& cmd)
	{
		fill_int_reply(ctx.reply, 1);
		return 0;
	}

	int ArdbServer::Expireat(ArdbConnContext& ctx, ArgumentArray& cmd)
	{
		fill_int_reply(ctx.reply, 1);
		return 0;
	}

	int ArdbServer::Exists(ArdbConnContext& ctx, ArgumentArray& cmd)
	{
		bool ret = m_db->Exists(ctx.currentDB, cmd[0]);
		fill_int_reply(ctx.reply, ret ? 1 : 0);
		return 0;
	}

	int ArdbServer::Del(ArdbConnContext& ctx, ArgumentArray& cmd)
	{
		SliceArray array;
		ArgumentArray::iterator it = cmd.begin();
		while (it != cmd.end())
		{
			array.push_back(*it);
			it++;
		}
		m_db->Del(ctx.currentDB, array);
		fill_int_reply(ctx.reply, array.size());
		return 0;
	}

	int ArdbServer::Set(ArdbConnContext& ctx, ArgumentArray& cmd)
	{
		const std::string& key = cmd[0];
		const std::string& value = cmd[1];
		int ret = 0;
		if (cmd.size() == 2)
		{
			ret = m_db->Set(ctx.currentDB, key, value);
		}
		else
		{
			int i = 0;
			uint64 px = 0, ex = 0;
			for (i = 2; i < cmd.size(); i++)
			{
				std::string tmp = string_tolower(cmd[i]);
				if (tmp == "ex" || tmp == "px")
				{
					int64 iv;
					if (!raw_toint64(cmd[i + 1].c_str(), cmd[i + 1].size(), iv)
					        || iv < 0)
					{
						fill_error_reply(ctx.reply,
						        "ERR value is not an integer or out of range");
						return 0;
					}
					if (tmp == "px")
					{
						px = iv;
					}
					else
					{
						ex = iv;
					}
					i++;
				}
				else
				{
					break;
				}
			}
			bool hasnx, hasxx;
			bool syntaxerror = false;
			if (i < cmd.size() - 1)
			{
				syntaxerror = true;
			}
			if (i == cmd.size() - 1)
			{
				std::string cmp = string_tolower(cmd[i]);
				if (cmp != "nx" && cmp != "xx")
				{
					syntaxerror = true;
				}
				else
				{
					hasnx = cmp == "nx";
					hasxx = cmp == "xx";
				}
			}
			if (syntaxerror)
			{
				fill_error_reply(ctx.reply, "ERR syntax error");
				return 0;
			}
			int nxx = 0;
			if (hasnx)
			{
				nxx = -1;
			}
			if (hasxx)
			{
				nxx = 1;
			}
			ret = m_db->Set(ctx.currentDB, key, value, ex, px, nxx);
		}
		if (0 == ret)
		{
			fill_status_reply(ctx.reply, "OK");
		}
		else
		{
			ctx.reply.type = REDIS_REPLY_NIL;
		}
		return 0;
	}

	int ArdbServer::Get(ArdbConnContext& ctx, ArgumentArray& cmd)
	{
		const std::string& key = cmd[0];
		std::string value;
		if (0 == m_db->Get(ctx.currentDB, key, &value))
		{
			FILL_STR_REPLY(ctx.reply, value);
		}
		else
		{
			ctx.reply.type = REDIS_REPLY_NIL;
		}
		return 0;
	}

	int ArdbServer::SetEX(ArdbConnContext& ctx, ArgumentArray& cmd)
	{
		uint32 secs;
		if (!string_touint32(cmd[1], secs))
		{
			fill_error_reply(ctx.reply,
			        "ERR value is not an integer or out of range");
			return 0;
		}
		m_db->SetEx(ctx.currentDB, cmd[0], cmd[2], secs);
		fill_status_reply(ctx.reply, "OK");
		return 0;
	}
	int ArdbServer::SetNX(ArdbConnContext& ctx, ArgumentArray& cmd)
	{
		int ret = m_db->SetNX(ctx.currentDB, cmd[0], cmd[1]);
		fill_int_reply(ctx.reply, ret);
		return 0;
	}
	int ArdbServer::SetRange(ArdbConnContext& ctx, ArgumentArray& cmd)
	{
		int32 offset;
		if (!string_toint32(cmd[1], offset))
		{
			fill_error_reply(ctx.reply,
			        "ERR value is not an integer or out of range");
			return 0;
		}
		int ret = m_db->SetRange(ctx.currentDB, cmd[0], offset, cmd[2]);
		fill_int_reply(ctx.reply, ret);
		return 0;
	}
	int ArdbServer::Strlen(ArdbConnContext& ctx, ArgumentArray& cmd)
	{
		int ret = m_db->Strlen(ctx.currentDB, cmd[0]);
		fill_int_reply(ctx.reply, ret);
		return 0;
	}

	int ArdbServer::SetBit(ArdbConnContext& ctx, ArgumentArray& cmd)
	{
		int32 offset, val;
		if (!string_toint32(cmd[1], offset))
		{
			fill_error_reply(ctx.reply,
			        "ERR value is not an integer or out of range");
			return 0;
		}
		if (cmd[2] != "1" && cmd[2] != "0")
		{
			fill_error_reply(ctx.reply,
			        "ERR bit is not an integer or out of range");
			return 0;
		}
		uint8 bit = cmd[2] != "1";
		int ret = m_db->SetBit(ctx.currentDB, cmd[0], offset, bit);
		fill_int_reply(ctx.reply, bit);
		return 0;
	}

	int ArdbServer::PSetEX(ArdbConnContext& ctx, ArgumentArray& cmd)
	{
		uint32 mills;
		if (!string_touint32(cmd[1], mills))
		{
			fill_error_reply(ctx.reply,
			        "ERR value is not an integer or out of range");
			return 0;
		}
		m_db->PSetEx(ctx.currentDB, cmd[0], cmd[2], mills);
		fill_status_reply(ctx.reply, "OK");
		return 0;
	}

	int ArdbServer::MSetNX(ArdbConnContext& ctx, ArgumentArray& cmd)
	{
		if (cmd.size() % 2 != 0)
		{
			fill_error_reply(ctx.reply,
			        "ERR wrong number of arguments for MSETNX");
			return 0;
		}
		SliceArray keys;
		SliceArray vals;
		for (int i = 0; i < cmd.size(); i += 2)
		{
			keys.push_back(cmd[i]);
			vals.push_back(cmd[i + 1]);
		}
		int count = m_db->MSetNX(ctx.currentDB, keys, vals);
		fill_int_reply(ctx.reply, count);
		return 0;
	}

	int ArdbServer::MSet(ArdbConnContext& ctx, ArgumentArray& cmd)
	{
		if (cmd.size() % 2 != 0)
		{
			fill_error_reply(ctx.reply,
			        "ERR wrong number of arguments for MSET");
			return 0;
		}
		SliceArray keys;
		SliceArray vals;
		for (int i = 0; i < cmd.size(); i += 2)
		{
			keys.push_back(cmd[i]);
			vals.push_back(cmd[i + 1]);
		}
		m_db->MSet(ctx.currentDB, keys, vals);
		fill_status_reply(ctx.reply, "OK");
		return 0;
	}

	int ArdbServer::MGet(ArdbConnContext& ctx, ArgumentArray& cmd)
	{
		SliceArray keys;
		for (int i = 0; i < cmd.size(); i++)
		{
			keys.push_back(cmd[i]);
		}
		ValueArray res;
		m_db->MGet(ctx.currentDB, keys, res);
		fill_array_reply(ctx.reply, res);
		return 0;
	}

	int ArdbServer::IncrbyFloat(ArdbConnContext& ctx, ArgumentArray& cmd)
	{
		double increment, val;
		if (!string_todouble(cmd[1], increment))
		{
			fill_error_reply(ctx.reply,
			        "ERR value is not a float or out of range");
			return 0;
		}
		int ret = m_db->IncrbyFloat(ctx.currentDB, cmd[0], increment, val);
		if (ret == 0)
		{
			char tmp[256];
			snprintf(tmp, sizeof(tmp) - 1, "%f", val);
			FILL_STR_REPLY(ctx.reply, tmp);
		}
		else
		{
			fill_error_reply(ctx.reply,
			        "ERR value is not a float or out of range");
		}
		return 0;
	}

	int ArdbServer::Incrby(ArdbConnContext& ctx, ArgumentArray& cmd)
	{
		int64 increment, val;
		if (!string_toint64(cmd[1], increment))
		{
			fill_error_reply(ctx.reply,
			        "ERR value is not an integer or out of range");
			return 0;
		}
		int ret = m_db->Incrby(ctx.currentDB, cmd[0], increment, val);
		if (ret == 0)
		{
			fill_int_reply(ctx.reply, val);
		}
		else
		{
			fill_error_reply(ctx.reply,
			        "ERR value is not an integer or out of range");
		}
		return 0;
	}

	int ArdbServer::Incr(ArdbConnContext& ctx, ArgumentArray& cmd)
	{
		int64_t val;
		int ret = m_db->Incr(ctx.currentDB, cmd[0], val);
		if (ret == 0)
		{
			fill_int_reply(ctx.reply, val);
		}
		else
		{
			fill_error_reply(ctx.reply,
			        "ERR value is not an integer or out of range");
		}
		return 0;
	}

	int ArdbServer::GetSet(ArdbConnContext& ctx, ArgumentArray& cmd)
	{
		std::string v;
		int ret = m_db->GetSet(ctx.currentDB, cmd[0], cmd[1], v);
		if (ret < 0)
		{
			ctx.reply.type = REDIS_REPLY_NIL;
		}
		else
		{
			FILL_STR_REPLY(ctx.reply, v);
		}
		return 0;
	}

	int ArdbServer::GetRange(ArdbConnContext& ctx, ArgumentArray& cmd)
	{
		int32 start, end;
		if (!string_toint32(cmd[1], start) || !string_toint32(cmd[2], end))
		{
			fill_error_reply(ctx.reply,
			        "ERR value is not an integer or out of range");
			return 0;
		}
		std::string v;
		m_db->GetRange(ctx.currentDB, cmd[0], start, end, v);
		FILL_STR_REPLY(ctx.reply, v);
		return 0;
	}

	int ArdbServer::GetBit(ArdbConnContext& ctx, ArgumentArray& cmd)
	{
		int32 offset;
		if (!string_toint32(cmd[1], offset))
		{
			fill_error_reply(ctx.reply,
			        "ERR value is not an integer or out of range");
			return 0;
		}
		int ret = m_db->GetBit(ctx.currentDB, cmd[0], offset);
		fill_int_reply(ctx.reply, ret);
		return 0;
	}

	int ArdbServer::Decrby(ArdbConnContext& ctx, ArgumentArray& cmd)
	{
		int64 decrement, val;
		if (!string_toint64(cmd[1], decrement))
		{
			fill_error_reply(ctx.reply,
			        "ERR value is not an integer or out of range");
			return 0;
		}
		int ret = m_db->Decrby(ctx.currentDB, cmd[0], decrement, val);
		if (ret == 0)
		{
			fill_int_reply(ctx.reply, val);
		}
		else
		{
			fill_error_reply(ctx.reply,
			        "ERR value is not an integer or out of range");
		}
		return 0;
	}

	int ArdbServer::Decr(ArdbConnContext& ctx, ArgumentArray& cmd)
	{
		int64_t val;
		int ret = m_db->Decr(ctx.currentDB, cmd[0], val);
		if (ret == 0)
		{
			fill_int_reply(ctx.reply, val);
		}
		else
		{
			fill_error_reply(ctx.reply,
			        "ERR value is not an integer or out of range");
		}
		return 0;
	}

	int ArdbServer::Bitop(ArdbConnContext& ctx, ArgumentArray& cmd)
	{
		SliceArray keys;
		for (int i = 2; i < cmd.size(); i++)
		{
			keys.push_back(cmd[i]);
		}
		int ret = m_db->BitOP(ctx.currentDB, cmd[0], cmd[1], keys);
		if (ret < 0)
		{
			fill_error_reply(ctx.reply, "ERR syntax error");
		}
		else
		{
			fill_int_reply(ctx.reply, ret);
		}
		return 0;
	}

	int ArdbServer::Bitcount(ArdbConnContext& ctx, ArgumentArray& cmd)
	{
		if (cmd.size() == 2)
		{
			fill_error_reply(ctx.reply, "ERR syntax error");
			return 0;
		}
		int count = 0;
		if (cmd.size() == 1)
		{
			count = m_db->BitCount(ctx.currentDB, cmd[0], 0, -1);
		}
		else
		{
			int32 start, end;
			if (!string_toint32(cmd[1], start) || !string_toint32(cmd[2], end))
			{
				fill_error_reply(ctx.reply,
				        "ERR value is not an integer or out of range");
				return 0;
			}
			count = m_db->BitCount(ctx.currentDB, cmd[0], start, end);
		}
		fill_int_reply(ctx.reply, count);
		return 0;
	}

	int ArdbServer::Append(ArdbConnContext& ctx, ArgumentArray& cmd)
	{
		const std::string& key = cmd[0];
		const std::string& value = cmd[1];
		int ret = m_db->Append(ctx.currentDB, key, value);
		if (ret > 0)
		{
			fill_int_reply(ctx.reply, ret);
		}
		else
		{
			fill_error_reply(ctx.reply, "ERR failed to append key:%s",
			        key.c_str());
		}
		return 0;
	}

	int ArdbServer::Ping(ArdbConnContext& ctx, ArgumentArray& cmd)
	{
		fill_status_reply(ctx.reply, "PONG");
		return 0;
	}
	int ArdbServer::Echo(ArdbConnContext& ctx, ArgumentArray& cmd)
	{
		ctx.reply.str = cmd[0];
		ctx.reply.type = REDIS_REPLY_STRING;
		return 0;
	}
	int ArdbServer::Select(ArdbConnContext& ctx, ArgumentArray& cmd)
	{
		ctx.currentDB = cmd[0];
		fill_status_reply(ctx.reply, "OK");
		DEBUG_LOG("Select db is %s", cmd[0].c_str());
		return 0;
	}

	int ArdbServer::Quit(ArdbConnContext& ctx, ArgumentArray& cmd)
	{
		fill_status_reply(ctx.reply, "OK");
		return -1;
	}

	int ArdbServer::Shutdown(ArdbConnContext& ctx, ArgumentArray& cmd)
	{
		m_service->Stop();
		return -1;
	}

	int ArdbServer::Slaveof(ArdbConnContext& ctx, ArgumentArray& cmd)
	{
		return 0;
	}

	void ArdbServer::ProcessRedisCommand(ArdbConnContext& ctx,
	        RedisCommandFrame& args)
	{
		ctx.reply.Clear();
		std::string* cmd = args.GetArgument(0);
		if (NULL != cmd)
		{
			int ret = 0;
			lower_string(*cmd);
			RedisCommandHandlerSettingTable::iterator found =
			        m_handler_table.find(*cmd);
			if (found != m_handler_table.end())
			{
				RedisCommandHandler handler = found->second.handler;
				args.GetArguments().pop_front();

				bool valid_cmd = true;
				if (found->second.min_arity >= 0)
				{
					valid_cmd = args.GetArguments().size()
					        >= found->second.min_arity;
				}
				if (found->second.max_arity >= 0 && valid_cmd)
				{
					valid_cmd = args.GetArguments().size()
					        <= found->second.max_arity;
				}

				if (!valid_cmd)
				{
					fill_error_reply(ctx.reply,
					        "ERR wrong number of arguments for '%s' command",
					        cmd->c_str());
				}
				else
				{
					ret = (this->*handler)(ctx, args.GetArguments());
				}

			}
			else
			{
				ERROR_LOG("No handler found for:%s", cmd->c_str());
				fill_error_reply(ctx.reply, "ERR unknown command '%s'",
				        cmd->c_str());
			}

			Buffer buf;
			if (ctx.reply.type != 0)
			{
				Buffer buf;
				encode_reply(buf, ctx.reply);
				ctx.conn->Write(buf);
			}
			if (ret < 0)
			{
				ctx.conn->Close();
			}
		}
	}

	static void ardb_pipeline_init(ChannelPipeline* pipeline, void* data)
	{
		ChannelUpstreamHandler<RedisCommandFrame>* handler =
		        (ChannelUpstreamHandler<RedisCommandFrame>*) data;
		pipeline->AddLast("decoder", new RedisFrameDecoder);
		pipeline->AddLast("handler", handler);
	}

	static void ardb_pipeline_finallize(ChannelPipeline* pipeline, void* data)
	{
		ChannelHandler* handler = pipeline->Get("decoder");
		DELETE(handler);
	}

	int ArdbServer::Start(const Properties& props)
	{
		ParseConfig(props, m_cfg);
		struct RedisRequestHandler: public ChannelUpstreamHandler<
		        RedisCommandFrame>
		{
				ArdbServer* server;
				ArdbConnContext ardbctx;
				void MessageReceived(ChannelHandlerContext& ctx,
				        MessageEvent<RedisCommandFrame>& e)
				{
					ardbctx.conn = ctx.GetChannel();
					server->ProcessRedisCommand(ardbctx, *(e.GetMessage()));
				}
				RedisRequestHandler(ArdbServer* s) :
						server(s)
				{
				}
		};
#ifdef __USE_KYOTOCABINET__
		m_engine = new KCDBEngineFactory(props);
#else
		m_engine = new LevelDBEngineFactory(props);
#endif
		m_db = new Ardb(m_engine);
		m_service = new ChannelService(m_cfg.max_clients + 32);
		RedisRequestHandler handler(this);

		ChannelOptions ops;
		ops.tcp_nodelay = true;
		if (m_cfg.listen_host.empty() && m_cfg.listen_unix_path.empty())
		{
			m_cfg.listen_host = "0.0.0.0";
			if (m_cfg.listen_port == 0)
			{
				m_cfg.listen_port = 6379;
			}
		}
		if (!m_cfg.listen_host.empty())
		{
			SocketHostAddress address(m_cfg.listen_host.c_str(),
			        m_cfg.listen_port);
			ServerSocketChannel* server = m_service->NewServerSocketChannel();
			if (!server->Bind(&address))
			{
				ERROR_LOG(
				        "Failed to bind on %s:%d", m_cfg.listen_host.c_str(), m_cfg.listen_port);
				return -1;
			}
			server->Configure(ops);
			server->SetChannelPipelineInitializor(ardb_pipeline_init, &handler);
			server->SetChannelPipelineFinalizer(ardb_pipeline_finallize, NULL);
		}
		if (!m_cfg.listen_unix_path.empty())
		{
			SocketUnixAddress address(m_cfg.listen_unix_path);
			ServerSocketChannel* server = m_service->NewServerSocketChannel();
			if (!server->Bind(&address))
			{
				ERROR_LOG(
				        "Failed to bind on %s", m_cfg.listen_unix_path.c_str());
				return -1;
			}
			server->Configure(ops);
			server->SetChannelPipelineInitializor(ardb_pipeline_init, &handler);
			server->SetChannelPipelineFinalizer(ardb_pipeline_finallize, NULL);
		}
		m_service->Start();
		DELETE(m_engine);
		DELETE(m_db);
		DELETE(m_service);
		return 0;
	}
}


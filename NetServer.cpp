#include "stdafx.h"
#include "Session.h"
#include "PrintView.h"
#include "Iocp.h"
#include "NetServer.h"

NetServer::NetServer()
	: accept_cnt_(0), recv_cnt_(0), send_cnt_(0), server_exit_(false)
{
	iocp_ = new Iocp;
}


NetServer::~NetServer()
{
	CloseHandle(iocp_->GetIocp());
	delete iocp_;
}

bool NetServer::Init(const SERVER_PARAM &server_param)
{
	session_num_	= SESSION_NUM;
	thread_num_		= server_param.thread_num;
	nagle_opt_		= server_param.nagle_opt;
	packet_code_	= server_param.packet_code;

	PacketMP packet;

	//***********************************************
	// �޸�Ǯ �ʱ�ȭ
	//***********************************************
	packet.MemPoolInit(PACKET_MEM_POOL_SIZE);
	session_connect_pool_.Init(session_num_);
	accept_que_.Init(ACCEPT_QUE_SIZE);

	if (!iocp_->Init(thread_num_))
		return false;

	if (!NetInit(server_param.port))
		return false;

	SessionArrInit();

	return true;
}

bool NetServer::NetInit(int port)
{
	listen_sock_ = WSASocket(AF_INET, SOCK_STREAM, NULL, NULL, 0, WSA_FLAG_OVERLAPPED);

	if (listen_sock_ == INVALID_SOCKET)
	{
		LOG(L"GAME_SERVER", LOG_ERROR, L"!!!LISTENSOCKET CREATE FAIL!!!");
		return false;
	}
	//***********************************************
	// SO_REUSEADDR ����
	//***********************************************
	bool opt_val = true;

	if (setsockopt(listen_sock_, SOL_SOCKET, SO_REUSEADDR, (char*)&opt_val, sizeof(opt_val)) == SOCKET_ERROR)
	{
		LOG(L"GAME_SERVER", LOG_ERROR, L"!!!SO_REUSEADDR ERROR!!!");
		return false;
	}

	SOCKADDR_IN	server_addr;

	ZeroMemory(&server_addr, sizeof(server_addr));

	server_addr.sin_family		= AF_INET;
	server_addr.sin_port		= htons(port);
	server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

	int net_ret = ::bind(listen_sock_, (SOCKADDR*)&server_addr, sizeof(server_addr));

	if (net_ret == SOCKET_ERROR)
	{
		LOG(L"GAME_SERVER", LOG_ERROR, L"!!!BIND ERROR!!!");
		return false;
	}

	net_ret = listen(listen_sock_, SOMAXCONN);

	if (net_ret == SOCKET_ERROR)
	{
		LOG(L"GAME_SERVER", LOG_ERROR, L"!!!LISTEN FAIL!!!");
		return false;
	}

	return true;
}

void NetServer::ThreadInit(void)
{
	//***********************************************
	// �����带 ���ʴ�� ����, �� WorkerThread �� 
	// ����� ������ �°� ����
	//***********************************************
	wstring thread_name;

	thread_name = L"Accept_Thread";
	__thread_manager.Push(new thread{ &NetServer::AcceptThread, this }, thread_name);
	thread_name.clear();

	thread_name = L"Auth_Thread";
	__thread_manager.Push(new thread{ &NetServer::AuthThread, this }, thread_name);
	thread_name.clear();

	thread_name = L"Update_Thread";
	__thread_manager.Push(new thread{ &NetServer::UpdateThread, this }, thread_name);
	thread_name.clear();

	thread_name = L"Send_Thread";
	__thread_manager.Push(new thread{ &NetServer::SendThread, this }, thread_name);
	thread_name.clear();

	thread_name = L"TpsUpdate_Thread";
	__thread_manager.Push(new thread{ &NetServer::TpsUpdateThread, this }, thread_name);
	thread_name.clear();

	for (int num = 1; num <= thread_num_; num++)
	{
		wstringstream numbering;

		numbering << num;
		thread_name = L"Worker_Thread";
		thread_name += numbering.str();

		__thread_manager.Push(new thread{ &NetServer::WorkerThread, this }, thread_name);

		thread_name.clear();
	}

	worker_flag_ = false;
}

void NetServer::SessionArrInit(void)
{
	session_blank_index_stack_	= new LockFreeStack<int>;
	
	ZeroMemory(session_arr_, sizeof(void*)*session_num_);

	session_blank_index_stack_->Init(session_num_);

	for (int num = session_num_ - 1; num >= 0; num--)
	{
		//*******************************************
		// ���� �迭���� ���߿� ����� �ε��� ���� �־��ش�.
		//*******************************************
		session_blank_index_stack_->Push(&num);
	}

	__print_view.session_blank_mem_size_.exchange(session_blank_index_stack_->MemUseSize());
	__print_view.session_blank_stack_size_.exchange(session_blank_index_stack_->UseSize());
}

void NetServer::Release(void)
{
	server_exit_ = true;

	DisconnectSock(&listen_sock_);

	//***********************************************
	// ������ �ð����� Accept ������ �ߴ� Ȯ��
	//***********************************************
	__thread_manager.SingleWait(L"Accept_Thread", 5000);

	//***********************************************
	// ���� ���� Shutdown Send ����
	//***********************************************
	ShutDown();

	//***********************************************
	// Worker ������ ����
	//***********************************************
	for (int num = 0; num < thread_num_; num++)
		PostQueuedCompletionStatus(iocp_->GetIocp(), 0, 0, NULL);

	//***********************************************
	// ������ �ð����� Worker ������ �ߴ� Ȯ��
	//***********************************************
	for (int num = 1; num <= thread_num_; num++)
	{
		wstring thread_name;
		wstringstream numbering;

		numbering << num;
		thread_name = L"Worker_Thread";
		thread_name += numbering.str();

		__thread_manager.SingleWait(thread_name, 5000);
	}

	//***********************************************
	// ������ �ð����� Auth ������ �ߴ� Ȯ��
	//***********************************************
	__thread_manager.SingleWait(L"Auth_Thread", 5000);

	//***********************************************
	// ������ �ð����� Update ������ �ߴ� Ȯ��
	//***********************************************
	__thread_manager.SingleWait(L"Update_Thread", 5000);

	//***********************************************
	// ������ �ð����� Send ������ �ߴ� Ȯ��
	//***********************************************
	__thread_manager.SingleWait(L"Send_Thread", 5000);

	//***********************************************
	// ������ �ð����� Ƽ�ǿ��� ������Ʈ ������ �ߴ� Ȯ��
	//***********************************************
	__thread_manager.SingleWait(L"Net_TpsUpdate_Thread", 5000);

	//***********************************************
	// ��ӹ��� Ŭ���� ������
	//***********************************************
	OnStop();

	//*******************************************
	// �޸�Ǯ ������
	//*******************************************
	session_connect_pool_.Release();
	accept_que_.Release();
	session_blank_index_stack_->Release();

	delete session_blank_index_stack_;
	
	for (int index = 0; index < session_num_; index++)
		session_arr_[index]->Release();

	ZeroMemory(session_arr_, sizeof(void*)*session_num_);

	WSACleanup();
}

void NetServer::DisconnectSock(SOCKET *sock)
{
	//***********************************************
	// �����ʿ��� �����Ƿ� TIME_WAIT �� ���� �ʵ��� ���� �ɼ��� 
	// �ɾ Ŭ���̾�Ʈ Disconnect
	//***********************************************
	LINGER linger;

	linger.l_onoff	= 1;
	linger.l_linger	= 0;

	closesocket(*sock);
	*sock = INVALID_SOCKET;
}

void NetServer::ShutDown(void)
{
	//***********************************************
	// ���� ť ����� üũ�ؼ� ������� �ʴ� ���ǵ��� shutdown
	// ���� 100ms �� ���� �ݺ��� 10���� ���鼭 ���带 �� �� ���� shutdown
	//***********************************************
	int		try_cnt			= 0;
	bool	all_shut_down	= true;
	Session *session		= nullptr;

	while (true)
	{
		for (int index = 0; index < session_num_; index++)
		{
			session = session_arr_[index];

			if (session->account_no_ != -1 && session->shut_down_ == false)
			{
				if (session->send_que_.UseSize() <= 0 || try_cnt > 10)
				{
					//***********************************************
					// ����� ���Ḧ ���� SD_SEND shutdown �� �ٽ� �����ϴ� �� ����
					//***********************************************
					if (try_cnt > 10)
					{
						LOG(L"NET_SERVER", LOG_DEBUG, L"!!!SHUT DOWN FAIL!!! ACCOUNT_NO: %d,SOCEKT: %d, SEND_SIZE: %d",
							session->account_no_, session->sock_, session->send_que_.UseSize());
					}
					session->Disconnect();
				}
				else
				{
					all_shut_down = false;
				}
			}
		}

		try_cnt++;

		//***********************************************
		// �������� ��� ���Ͽ� ���ؼ� shutdown(SD_SEND) �Ϸ�� ����
		//***********************************************
		if (all_shut_down || try_cnt > 10)
			break;

		Sleep(100);
	}
}

void NetServer::AcceptThread(void)
{
	__print_view.accept_thread_num_++;
	//*******************************************
	// Keep Alive ����
	// ������ �������� ���� ������ ����
	//*******************************************
	tcp_keepalive tcp_kl;

	//*******************************************
	// keepalive on
	// ������ �ð���ŭ keepalive ��ȣ ���� (������ �⺻ 2�ð�)
	// keepalive ��ȣ�� ������ ������ ������ 1�ʸ��� �� ���� (ms tcp �� 10ȸ ��õ�)
	//*******************************************
	tcp_kl.onoff				= 1;
	tcp_kl.keepalivetime		= KEEP_ALIVE_TIME;
	tcp_kl.keepaliveinterval	= KEEP_ALIVE_INTERVAL;

	LONG64	account_no			= 1;
	bool	accept_err			= false;

	while (!server_exit_)
	{
		if (!SessionAccpet(&tcp_kl, account_no++))
			break;

		accept_cnt_++;
	}

	__print_view.accept_thread_num_--;
}

bool NetServer::SessionAccpet(tcp_keepalive *tcp_kl, LONG64 account_no)
{
	//*******************************************
	// ����ڰ� �ִ�ġ�� ���� �� ������ ����
	//*******************************************
	if (session_blank_index_stack_->UseSize() > session_num_)
		return true;

	SOCKADDR_IN session_addr;
	SOCKET		session_sock;
	int			addr_len;
	int			session_port;

	ZeroMemory(&session_addr, sizeof(session_addr));

	addr_len		= sizeof(session_addr);
	session_sock	= WSAAccept(listen_sock_, (SOCKADDR*)&session_addr, &addr_len, NULL, 0);
	session_sock	= session_sock == INVALID_SOCKET ? INVALID_SOCKET : session_sock;
	session_port	= ntohs(session_addr.sin_port);

	wstring session_ip;
	
	session_ip.resize(16);

	if (session_sock != INVALID_SOCKET)
	{
		InetNtop(AF_INET, &session_addr.sin_addr, (PWSTR)session_ip.data(), sizeof(session_ip));
	}
	else
	{
		LOG(L"NET_SERVER", LOG_ERROR, L"!!!ACCEPT INVALID_SOCKET!!!");
		return false;
	}

	bool opt_val = nagle_opt_;

	//*******************************************
	// Nagle �˰��� ����
	//*******************************************
	if (setsockopt(session_sock, IPPROTO_TCP, TCP_NODELAY, (char*)&opt_val, sizeof(opt_val)) == SOCKET_ERROR)
	{
		LOG(L"NET_SERVER", LOG_ERROR, L"!!!NAGLE ERROR!!!");
		return false;
	}

	//*******************************************
	// Keep Alive �ɼ� ����
	//*******************************************
	if (setsockopt(session_sock, SOL_SOCKET, SO_KEEPALIVE, (char *)&tcp_kl->onoff, sizeof(tcp_kl->onoff)) == SOCKET_ERROR)
	{
		LOG(L"NET_SERVER", LOG_ERROR, L"!!!KEEPALIVE ERROR!!!");
		return false;
	}

	DWORD ret_val;

	WSAIoctl(session_sock, SIO_KEEPALIVE_VALS, tcp_kl, sizeof(tcp_keepalive), 0, 0, &ret_val, NULL, NULL);

	SESSION_CONNECT_PARAM *connect_param = nullptr;

	connect_param = session_connect_pool_.Alloc();

	connect_param->sock			= session_sock;
	connect_param->account_no	= account_no;

	//*******************************************
	// Accept �۾��� ������ �ش� ������ 
	// accept_que �� Push
	// ���� ť�� Ǯ�̸� ���� ó�� �� �޸� �ݳ�
	//*******************************************
	if (accept_que_.Push(&connect_param))
	{
		__print_view.accept_session_num_.fetch_add(1);
	}
	else
	{
		DisconnectSock(&session_sock);
		session_connect_pool_.Free(connect_param);
	}

	return true;
}

void NetServer::AuthThread(void)
{
	__print_view.auth_thread_num_++;

	while (!server_exit_)
	{
		//*******************************************
		// 1. �ű� ������ ó��
		//*******************************************
		AuthAccpet();

		//*******************************************
		// 2. ������ ���� ó��
		//*******************************************
		AuthProc();

		//*******************************************
		// 3. ������ �α׾ƿ� ó�� 
		//*******************************************
		AuthLogout();

		Sleep(THREAD_AUTH_SLEEP);
	}

	__print_view.auth_thread_num_--;
}

void NetServer::AuthAccpet(void)
{
	int						index;
	SESSION_CONNECT_PARAM	*connect_param;

	while (accept_que_.Pop(&connect_param))
	{
		//*******************************************
		// �ε��� ���ÿ��� ����ִ� ������ ������
		//*******************************************
		if (!session_blank_index_stack_->Pop(&index))
		{
			SOCKET sock = connect_param->sock;
			DisconnectSock(&sock);
		}
		else
		{
			//*******************************************
			// ���� ��� �� ���� ���� �ʱ�ȭ 
			//*******************************************
			Session *session			= session_arr_[index];
			connect_param->arr_index	= index;

			session->ConnectInit(*connect_param);
			
			//*******************************************
			// ������ Iocp �ʱ�ȭ�� �����̸� MODE_AUTH ��ȯ
			// �������̶�� ����ó��
			//*******************************************
			if (iocp_->IocpIng(session, thread_num_))
			{
				InterlockedExchange64(&session->io_cnt, 1);

				session->mode_			= Session::MODE_AUTH;
				session->logout_state_	= false;


				//*******************************************
				// Recv �ʱ�ȭ�� �������� ��� logout_state true 
				// ��ȯ �� Update Thread ���� �α׾ƿ� ó��
				//*******************************************
				if (!iocp_->RecvInit(session))
				{
					if (!session->ReleaseCheck())
						session->logout_state_ = true;
				}

				__print_view.session_num_.fetch_add(1);
				__print_view.auth_sesison_num_.fetch_add(1);
			}
			else
			{
				SOCKET sock = connect_param->sock;
				DisconnectSock(&sock);

				session_blank_index_stack_->Push(&session->arr_index_);
			}
		}

		session_connect_pool_.Free(connect_param);

		__print_view.accept_session_num_.fetch_sub(1);
		__print_view.session_blank_mem_size_.exchange(session_blank_index_stack_->MemUseSize());
		__print_view.session_blank_stack_size_.exchange(session_blank_index_stack_->UseSize());
	}
}

void NetServer::AuthProc(void)
{
	Session *session = nullptr;
	PacketMP *packet = nullptr;

	for (int num = 0; num < session_num_; num++)
	{
		session = session_arr_[num];

		if (session->mode_ != Session::MODE_AUTH)
			continue;

		int proc_cnt = PACKET_PROC_LOOP;

		//*******************************************
		// �ϳ��� ���ǿ� �ʹ� ���� �ð��� �ο��� �� �����Ƿ�
		// ��Ŷ�� ������ Ƚ����ŭ�� ����
		//*******************************************
		while (proc_cnt-- > 0)
		{
			if (!session->recv_complete_que_.Pop(&packet))
				break;

			OnAuthPacketRecv(session, packet);
			OnAuthPacketSend(session, packet);

			packet->Free();
		}

		if (session->logout_state_)
		{
			//*******************************************
			// WAIT_LOGOUT ���� ��ȯ �� UpdateThread ���� ���� ������
			//*******************************************
			if (session->send_io_ == Session::COMPLETE)
			{
				session->mode_ = Session::MODE_WAIT_LOGOUT;

				__print_view.auth_sesison_num_.fetch_sub(1);
			}

			continue;
		}

		//*******************************************
		// AUTH ó���� ������ ���� �ʱ� ���� ����
		//*******************************************
		OnAuthClientJoin(session);

		__print_view.auth_sesison_num_.fetch_sub(1);
		__print_view.game_session_num_.fetch_add(1);
	}
}

void NetServer::AuthLogout(void)
{
	Session *session = nullptr;

	for (int num = 0; num < session_num_; num++)
	{
		session = session_arr_[num];
		//*******************************************
		// WAIT_LOGOUT ���� ��ȯ �� UpdateThread ���� ���� ������
		//*******************************************
		if (session->mode_ == Session::MODE_LOGOUT_IN_AUTH && session->send_io_ == Session::COMPLETE)
		{
			session->mode_ = Session::MODE_WAIT_LOGOUT;

			__print_view.auth_sesison_num_.fetch_sub(1);
		}
	}
}

void NetServer::UpdateThread(void)
{
	__print_view.update_thread_num_++;

	while (!server_exit_)
	{
		//*******************************************
		// 1. ������ ���� ó��
		//*******************************************
		OnGameUpdate();

		//*******************************************
		// 2. ������ ���� ó��
		//*******************************************
		UpdateProc();
		
		//*******************************************
		// 3. ������ �α׾ƿ� ó�� 
		//*******************************************
		UpdateLogout();

		//*******************************************
		// 3. ������ ����
		//*******************************************
		UpdateRelease();
	}

	__print_view.update_thread_num_--;
}

void NetServer::UpdateProc(void)
{
	Session *session = nullptr;
	PacketMP *packet = nullptr;

	for (int num = 0; num < session_num_; num++)
	{
		session = session_arr_[num];

		if (session->mode_ == Session::MODE_AUTH_TO_GAME)
		{
			OnGameClientJoin(session);
			continue;
		}

		if (session->mode_ != Session::MODE_GAME)
			continue;

		int proc_cnt = PACKET_PROC_LOOP;

		//*******************************************
		// �ϳ��� ���ǿ� �ʹ� ���� �ð��� �ο��� �� �����Ƿ�
		// ��Ŷ�� ������ Ƚ����ŭ�� ����
		//*******************************************
		while (proc_cnt-- > 0)
		{
			if (!session->recv_complete_que_.Pop(&packet))
				break;

			OnGamePacketRecv(session, packet);

			OnGamePacketSend(session, packet);

			packet->Free();
		}

		if (session->logout_state_)
		{
			if (session->send_io_ == Session::COMPLETE)
			{
				OnGameLogout(session);

				session->mode_ = Session::MODE_WAIT_LOGOUT;

				__print_view.game_session_num_.fetch_sub(1);
			}
		}
	}
}

void NetServer::UpdateLogout(void)
{
	Session *session = nullptr;

	for (int num = 0; num < session_num_; num++)
	{
		session = session_arr_[num];

		//*******************************************
		// ���°� GAME LOGOUT �̸� WAIT LOGOUT ���� ���� 
		//*******************************************
		if (session->mode_ == Session::MODE_LOGOUT_IN_GAME && session->send_io_ == Session::COMPLETE)
		{
			OnGameLogout(session);

			session->mode_ = Session::MODE_WAIT_LOGOUT;

			__print_view.game_session_num_.fetch_sub(1);
		}
	}
}

void NetServer::UpdateRelease(void)
{
	Session *session = nullptr;

	for (int num = 0; num < session_num_; num++)
	{
		session = session_arr_[num];

		//*******************************************
		// WAIT_LOGOUT, send_io COMPLETE ������ ������ ���� ������ ó��
		//*******************************************
		if (session->mode_ == Session::MODE_WAIT_LOGOUT && session->send_io_ == Session::COMPLETE)
		{
			if (session->sock_ != INVALID_SOCKET)
				session->Linger();

			session->Release();
			session_blank_index_stack_->Push(&session->arr_index_);

			__print_view.session_num_.fetch_sub(1);
			__print_view.session_blank_mem_size_.exchange(session_blank_index_stack_->MemUseSize());
			__print_view.session_blank_stack_size_.exchange(session_blank_index_stack_->UseSize());
		}
	}
}

void NetServer::WorkerThread(void)
{
	__print_view.worker_thread_num_.fetch_add(1);

	LPOVERLAPPED		overlap;
	Session				*session;
	HANDLE				server_iocp;
	DWORD				transfer;

	OVERLAPPED_ENTRY	entry[ENTRY_NUM];
	ULONG				entry_remove;
	DWORD				success;

	server_iocp		= iocp_->GetIocp();

	int entry_num	= 0;

	while (!worker_flag_)
	{
		success = GetQueuedCompletionStatusEx(server_iocp, entry, ENTRY_NUM, &entry_remove, INFINITE, false);

		for (int entry_num = 0; entry_num < (int)entry_remove; entry_num++)
		{
			session = (Session*)entry[entry_num].lpCompletionKey;
			overlap = entry[entry_num].lpOverlapped;
			transfer = entry[entry_num].dwNumberOfBytesTransferred;

			//***********************************************
			// Iocp ������ ��Ŀ������ ������ ����
			//***********************************************
			if (success == false && session == nullptr)
			{
				worker_flag_ = true;

				__print_view.worker_thread_num_.fetch_sub(1);

				LOG(L"NET_SERVER", LOG_ERROR, L"!!!WORKER THREAD ERROR!!!");
				__dump.Crash();
				return;
			}

			//***********************************************
			// �˴� NULL �� ������ ���Ḧ ���� �̺�Ʈ.
			//***********************************************
			if (transfer == 0 && overlap == nullptr && session == nullptr)
			{
				__print_view.worker_thread_num_.fetch_sub(1);
				return;
			}


			if (success == false)
			{
				SuccessFalse(session);
			}

			//***********************************************
			// transfer �� 0 �� ��� ���� �������� ���� ����̹Ƿ� 
			// �ϴ� Disconnect ó��
			//
			// ����, 0 �������� ����ó���� ������ ���ָ� ���� �� 
			// Send �Ϸ� �ڵ尡 ó���ɼ� ����
			//***********************************************
			if (transfer == 0)
			{
				session->Disconnect();
			}

			if (&session->recv_overlap_ == overlap)
			{
				RecvComplete(session, transfer);
			}

			if (&session->send_overlap_ == overlap)
			{
				SendComplete(session, transfer);
			}
			
			if (!session->ReleaseCheck())
			{
				session->logout_state_ = true;
			}
		}
	}
}

void NetServer::SuccessFalse(Session *session)
{
	DWORD err = WSAGetLastError();

	if (session != nullptr)
	{
		if (err == ERROR_NETNAME_DELETED)
		{
			//***********************************************
			// �ش� ������ �̹� ����������(closesocket)
			// �������� ������ ���� ����.
			//***********************************************
		}
		else
		{
			//***********************************************
			// �� �ۿ� Ű�� �����ϸ� �����ϸ� ������ ���� ������ ����
			// ��κ� ����ó�� �ϴ� �ɷ� ����
			//***********************************************
		}
		LOG(L"NET_SERVER", LOG_DEBUG, L"!!!SOCKET ERROR : %d SUCCESS FALSE!!!", err);
		session->Disconnect();
		return;
	}
}

bool NetServer::RecvComplete(Session *session, int transfer)
{
	InterlockedIncrement64(&session->io_cnt);

	session->transfer_ = transfer;

	//***********************************************
	// transfer ��ŭ ���ú� ť �� rear ���� ����
	//***********************************************
	session->recv_que_.MoveRearPos(transfer);

	RecvDataProc(session);

	if (!iocp_->Recv(session))
	{
		InterlockedDecrement64(&session->io_cnt);
		return false;
	}

	return true;
}

void NetServer::RecvDataProc(Session *session)
{
	while (true)
	{
		if (!session->RecvDataProc())
			break;

		InterlockedIncrement64(&recv_cnt_);
	}
}

bool NetServer::SendComplete(Session* session, int transfer)
{
	PacketMP* packet[WSA_BUF_SIZE] = { nullptr };

	if (session->send_byte_ != transfer)
	{
		//************************************************
		// ������ ��Ŷ ������ �ٸ��� ���� ����
		// ���� Ŭ�� ť�� �� á�� �� �߻�
		//************************************************
		LOG(L"NET_SERVER", LOG_ERROR, L"!!!SEND ERROR!!! ACCOUNT_NO: %d, Send Byte[%d] is different from the our Send Byte[%d]",
			session->account_no_, transfer, session->send_byte_);

		return false;
	}

	//************************************************
	// WSASEND �� ��Ŷ��ŭ Send Que ���� ���� Free
	//************************************************
	for (int index = 0; index < session->send_num_; index++)
	{
		session->send_que_.Pop(&packet[index]);

		//************************************************
		// ��Ŷ�� nullptr �� ��Ȳ�̸� ���� ť ����!
		// ����� ���� ���� ��� ����
		//***********************************************
		if (packet[index] == nullptr)
		{
			LOG(L"NET_SERVER", LOG_ERROR, L"!!!SEND QUE ERROR!!! ACCOUNT_NO: %d, SESSION SEND QUE SIZE: %d!!!",
				session->account_no_, session->send_que_.UseSize());

			__dump.Crash();
			return false;
		}
		packet[index]->Free();

		InterlockedIncrement64(&send_cnt_);
	}

	InterlockedExchange((LONG*)&session->send_io_, (LONG)Session::COMPLETE);

	return true;
}

void NetServer::SendThread(void)
{
	__print_view.send_thread_num_++;

	Session *session = nullptr;

	while (!server_exit_)
	{
		for (int num = 0; num < session_num_; num++)
		{
			session = session_arr_[num];

			if (!session->SendStateCheck())
				continue;

			//***********************************************
			// Worker, Send �����忡�� io_cnt ���� �����ϹǷ�
			// Send �� �����ϰ� �Ǹ� ���ο��� ������ ó���� �غ�
			//***********************************************
			InterlockedIncrement64(&session->io_cnt);

			if (!iocp_->Send(session))
			{
				if (!session->ReleaseCheck())
				{
					session->logout_state_ = true;
				}
				InterlockedExchange((LONG*)&session->send_io_, (LONG)Session::COMPLETE);
			}
		}

		Sleep(THREAD_SEND_SLEEP);
	}

	__print_view.send_thread_num_--;
}

void NetServer::SetSessionArr(int index, Session* session)
{
	session_arr_[index] = session;
	session_arr_[index]->Init(packet_code_);
}

void NetServer::TpsUpdateThread(void)
{
	__print_view.tps_update_thread_num_++;

	while (!server_exit_)
	{
		Sleep(1000);

		__print_view.accept_tps_	= accept_cnt_;
		__print_view.recv_tps_		= recv_cnt_;
		__print_view.send_tps_		= send_cnt_;

		accept_cnt_ = 0;
		recv_cnt_	= 0;
		send_cnt_	= 0;
	}

	__print_view.tps_update_thread_num_--;
}
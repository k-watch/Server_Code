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
	// 메모리풀 초기화
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
	// SO_REUSEADDR 적용
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
	// 스레드를 차례대로 생성, 단 WorkerThread 는 
	// 사용자 개수에 맞게 생성
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
		// 세션 배열에서 나중에 사용할 인덱스 값을 넣어준다.
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
	// 지정한 시간까지 Accept 스레드 중단 확인
	//***********************************************
	__thread_manager.SingleWait(L"Accept_Thread", 5000);

	//***********************************************
	// 세션 소켓 Shutdown Send 중지
	//***********************************************
	ShutDown();

	//***********************************************
	// Worker 스레드 중지
	//***********************************************
	for (int num = 0; num < thread_num_; num++)
		PostQueuedCompletionStatus(iocp_->GetIocp(), 0, 0, NULL);

	//***********************************************
	// 지정한 시간까지 Worker 스레드 중단 확인
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
	// 지정한 시간까지 Auth 스레드 중단 확인
	//***********************************************
	__thread_manager.SingleWait(L"Auth_Thread", 5000);

	//***********************************************
	// 지정한 시간까지 Update 스레드 중단 확인
	//***********************************************
	__thread_manager.SingleWait(L"Update_Thread", 5000);

	//***********************************************
	// 지정한 시간까지 Send 스레드 중단 확인
	//***********************************************
	__thread_manager.SingleWait(L"Send_Thread", 5000);

	//***********************************************
	// 지정한 시간까지 티피에스 업데이트 스레드 중단 확인
	//***********************************************
	__thread_manager.SingleWait(L"Net_TpsUpdate_Thread", 5000);

	//***********************************************
	// 상속받은 클래스 릴리즈
	//***********************************************
	OnStop();

	//*******************************************
	// 메모리풀 릴리즈
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
	// 서버쪽에서 끊으므로 TIME_WAIT 이 남지 않도록 링거 옵션을 
	// 걸어서 클라이언트 Disconnect
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
	// 센드 큐 사이즈를 체크해서 사용하지 않는 세션들을 shutdown
	// 다음 100ms 를 쉬고 반복문 10번을 돌면서 센드를 다 한 세션 shutdown
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
					// 우아한 종료를 위해 SD_SEND shutdown 후 다시 접속하는 걸 방지
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
		// 접속중인 모든 소켓에 대해서 shutdown(SD_SEND) 완료시 중지
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
	// Keep Alive 적용
	// 물리적 차단으로 연결 끊김을 감지
	//*******************************************
	tcp_keepalive tcp_kl;

	//*******************************************
	// keepalive on
	// 지정된 시간만큼 keepalive 신호 전송 (윈도우 기본 2시간)
	// keepalive 신호를 보내고 응답이 없으면 1초마다 재 전송 (ms tcp 는 10회 재시도)
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
	// 사용자가 최대치면 받을 수 없으니 리턴
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
	// Nagle 알고리즘 여부
	//*******************************************
	if (setsockopt(session_sock, IPPROTO_TCP, TCP_NODELAY, (char*)&opt_val, sizeof(opt_val)) == SOCKET_ERROR)
	{
		LOG(L"NET_SERVER", LOG_ERROR, L"!!!NAGLE ERROR!!!");
		return false;
	}

	//*******************************************
	// Keep Alive 옵션 적용
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
	// Accept 작업이 끝나면 해당 정보를 
	// accept_que 에 Push
	// 만약 큐가 풀이면 종료 처리 후 메모리 반납
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
		// 1. 신규 접속자 처리
		//*******************************************
		AuthAccpet();

		//*******************************************
		// 2. 접속자 접속 처리
		//*******************************************
		AuthProc();

		//*******************************************
		// 3. 접속자 로그아웃 처리 
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
		// 인덱스 스택에서 비어있는 공간을 꺼내옴
		//*******************************************
		if (!session_blank_index_stack_->Pop(&index))
		{
			SOCKET sock = connect_param->sock;
			DisconnectSock(&sock);
		}
		else
		{
			//*******************************************
			// 세션 등록 후 접속 정보 초기화 
			//*******************************************
			Session *session			= session_arr_[index];
			connect_param->arr_index	= index;

			session->ConnectInit(*connect_param);
			
			//*******************************************
			// 소켓이 Iocp 초기화가 정상이면 MODE_AUTH 전환
			// 비정상이라면 종료처리
			//*******************************************
			if (iocp_->IocpIng(session, thread_num_))
			{
				InterlockedExchange64(&session->io_cnt, 1);

				session->mode_			= Session::MODE_AUTH;
				session->logout_state_	= false;


				//*******************************************
				// Recv 초기화가 비정상일 경우 logout_state true 
				// 전환 후 Update Thread 에서 로그아웃 처리
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
		// 하나의 세션에 너무 많은 시간을 부여할 수 없으므로
		// 패킷은 정해진 횟수만큼만 추출
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
			// WAIT_LOGOUT 으로 변환 후 UpdateThread 에서 최종 릴리즈
			//*******************************************
			if (session->send_io_ == Session::COMPLETE)
			{
				session->mode_ = Session::MODE_WAIT_LOGOUT;

				__print_view.auth_sesison_num_.fetch_sub(1);
			}

			continue;
		}

		//*******************************************
		// AUTH 처리가 끝나면 세션 초기 정보 셋팅
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
		// WAIT_LOGOUT 으로 변환 후 UpdateThread 에서 최종 릴리즈
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
		// 1. 접속자 게임 처리
		//*******************************************
		OnGameUpdate();

		//*******************************************
		// 2. 접속자 접속 처리
		//*******************************************
		UpdateProc();
		
		//*******************************************
		// 3. 접속자 로그아웃 처리 
		//*******************************************
		UpdateLogout();

		//*******************************************
		// 3. 접속자 해제
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
		// 하나의 세션에 너무 많은 시간을 부여할 수 없으므로
		// 패킷은 정해진 횟수만큼만 추출
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
		// 상태가 GAME LOGOUT 이면 WAIT LOGOUT 으로 변경 
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
		// WAIT_LOGOUT, send_io COMPLETE 상태인 세션을 최종 릴리즈 처리
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
			// Iocp 오류로 워커스레드 비정상 종료
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
			// 죄다 NULL 이 스레드 종료를 위한 이벤트.
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
			// transfer 가 0 일 경우 접속 끊김으로 오는 경우이므로 
			// 일단 Disconnect 처리
			//
			// 에러, 0 전송으로 종료처리를 무작정 해주면 종료 후 
			// Send 완료 코드가 처리될수 있음
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
			// 해당 소켓은 이미 끊어졌으니(closesocket)
			// 정상적인 종료라고 봐도 무방.
			//***********************************************
		}
		else
		{
			//***********************************************
			// 그 밖에 키가 존재하며 실패하면 에러로 인한 비정상 종료
			// 대부분 종료처리 하는 걸로 끝냄
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
	// transfer 만큼 리시브 큐 의 rear 값을 증가
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
		// 전송한 패킷 개수가 다르면 접속 해제
		// 보통 클라 큐가 다 찼을 때 발생
		//************************************************
		LOG(L"NET_SERVER", LOG_ERROR, L"!!!SEND ERROR!!! ACCOUNT_NO: %d, Send Byte[%d] is different from the our Send Byte[%d]",
			session->account_no_, transfer, session->send_byte_);

		return false;
	}

	//************************************************
	// WSASEND 한 패킷만큼 Send Que 에서 꺼내 Free
	//************************************************
	for (int index = 0; index < session->send_num_; index++)
	{
		session->send_que_.Pop(&packet[index]);

		//************************************************
		// 패킷이 nullptr 인 상황이면 서버 큐 문제!
		// 현재는 서버 종료 대신 덤프
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
			// Worker, Send 스레드에서 io_cnt 값을 조절하므로
			// Send 가 실패하게 되면 내부에서 릴리즈 처리를 준비
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
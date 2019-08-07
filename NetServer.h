#pragma once

class Iocp;

//***********************************************
// SERVER_PARAM:			서버 실행시 필요한 인자
//***********************************************
struct SERVER_PARAM
{
	wstring					ip;
	bool					nagle_opt;
	int						port;
	int						thread_num;

	PacketAD::
		PACKET_CODE			packet_code;

	DBConnector::
		DB_PARAM			db_param;
};

//***********************************************
// NetServer
// 서버 통신 모듈
//***********************************************
class NetServer
{
public:
	explicit					NetServer();
	virtual						~NetServer();

	//***********************************************
	// Isnitialize Funtion
	//***********************************************
	bool						Init(const SERVER_PARAM &server_param);
	bool						NetInit(int port);
	void						ThreadInit(void);
	void						SessionArrInit(void);
	//***********************************************

	//***********************************************
	// Release Funtion
	//***********************************************
	void						Release(void);
	void						DisconnectSock(SOCKET *sock);
	//***********************************************
	// ShutDown:				세션 소켓 Shutdown Send 중지
	//***********************************************
	void						ShutDown(void);
	//***********************************************

	//***********************************************
	// Accept Thread Funtion
	//***********************************************
	void						AcceptThread(void);
	bool						SessionAccpet(tcp_keepalive *tcp_kl, LONG64 account_no);
	//***********************************************

	//***********************************************
	// Auth Thread Funtion
	//***********************************************
	void						AuthThread(void);
	//***********************************************

	//***********************************************
	void						AuthAccpet(void);
	//***********************************************

	//***********************************************
	void						AuthProc(void);
	//***********************************************

	//***********************************************
	void						AuthLogout(void);

	//***********************************************
	// Update Thread Funtion
	//***********************************************
	void						UpdateThread(void);
	void						UpdateProc(void);
	void						UpdateLogout(void);
	void						UpdateRelease(void);


	//***********************************************
	// Worker Thread Funtion
	//***********************************************
	void						WorkerThread(void);
	void						SuccessFalse(Session *session);
	//***********************************************
	// Recv:					리시브 큐 데이터 확인 후
	//							WSARECV 등록
	//***********************************************
	bool						RecvComplete(Session *session, int transfer);
	//***********************************************
	// RecvDataProc:			리시브 큐 데이터 분해
	//***********************************************
	void						RecvDataProc(Session *session);
	////***********************************************
	// SendComplete:			WSASEND 완료 후 패킷 처리
	//***********************************************
	bool						SendComplete(Session* session, int transfer);
	//***********************************************

	//***********************************************
	// Send Thread Funtion
	//***********************************************
	void						SendThread(void);
	//***********************************************

	//***********************************************
	// TpsUpdateThread:			1초 당 Accept, Recv, Send 수 체크
	//***********************************************
	void						TpsUpdateThread(void);

	void						SetSessionArr(int index, Session* session);

	bool						GetNagleOpt(void) { return nagle_opt_; };
	void						SetNagleOpt(void) { nagle_opt_ = !nagle_opt_; };

protected:
	enum SERVER_INFO
	{
		SESSION_NUM = 4000,
	};

	//***********************************************
	// Virtual Funtion
	//***********************************************
	// OnAuthClientJoin:		최초 AUTH_MODE 클라이언트 처리
	//***********************************************
	virtual void				OnAuthClientJoin(Session *session)						= 0;
	//***********************************************
	// OnAuthPacketRecv:		AUTH_MODE 세션 리시브 패킷 처리
	//***********************************************
	virtual void				OnAuthPacketRecv(Session *session, PacketMP *packet)	= 0;
	//***********************************************
	// OnAuthPacketSend:		AUTH_MODE 세션 센드 패킷 처리
	//***********************************************
	virtual void				OnAuthPacketSend(Session *session, PacketMP *packet)	= 0;
	//***********************************************
	// OnGameUpdate:			게임 내 업데이트 처리
	//***********************************************
	virtual void				OnGameUpdate(void)										= 0;
	//***********************************************
	// OnAuthClientJoin:		MODE_AUTH_TO_GAME 클라이언트 처리
	//***********************************************
	virtual void				OnGameClientJoin(Session *session)						= 0;
	//***********************************************
	// OnGamePacketRecv:		GAME_MODE 세션 리시브 패킷 처리
	//***********************************************
	virtual void				OnGamePacketRecv(Session *session, PacketMP *packet)	= 0;
	//***********************************************
	// OnGamePacketSend:		GAME_MODE 세션 센드 패킷 처리
	//***********************************************
	virtual void				OnGamePacketSend(Session *session, PacketMP *packet)	= 0;
	//***********************************************
	// OnGameLogout:			게임 로그아웃 패킷 처리
	//***********************************************
	virtual void				OnGameLogout(Session *session)							= 0;
	//***********************************************
	// OnSendUnicast:			해당 세션 Send 호출
	//***********************************************
	virtual void				OnSendUnicast(Session *session, PacketMP *packet)		= 0;
	//***********************************************
	// OnRecv:					패킷 리시브 후 이벤트
	//***********************************************
	virtual void				OnRecv(const UPDATE_PARAM &update_param) = 0;
	//***********************************************
	// OnHeartbeat:				하트비트 전송
	//***********************************************
	virtual void				OnHeartbeat(const THREAD_TYPE &thread_type) = 0;
	// ***********************************************
	// OnStop:					서버 종료 직전 이벤트
	// ***********************************************
	virtual void				OnStop(void) = 0;
	//***********************************************

	//***********************************************
	// Thread Value
	//***********************************************
	HANDLE						update_event_;
	HANDLE						db_event_;
	bool						worker_flag_;
	bool						update_flag_;
	bool						db_flag_;
	//***********************************************

	//***********************************************
	// session_arr_:			세션 관리 배열
	//***********************************************
	Session						*session_arr_[SESSION_NUM];
	int							session_num_;
	PacketAD::
		PACKET_CODE				packet_code_;

private:
	//***********************************************
	// Server Init Value
	//***********************************************
	SOCKET						listen_sock_;
	bool						nagle_opt_;
	int							thread_num_;
	//***********************************************

	//***********************************************
	// Accept Thread Value
	//***********************************************
	LONG64						accept_cnt_;
	LONG64						recv_cnt_;
	LONG64						send_cnt_;
	//***********************************************

	//***********************************************
	// server_exit:				서버 종료 플래그
	//***********************************************
	bool						server_exit_;

	//***********************************************
	// accept_que_:				Accept 완료된 세션 관리 큐
	// session_connect_pool_:	새로 Accept 할 동적 세션 관리
	// blank_index_stack_:		세션 배열 인덱스 관리 스택
	//***********************************************
	LockQueue<SESSION_CONNECT_PARAM*> 	accept_que_;
	LockStack<SESSION_CONNECT_PARAM>	session_connect_pool_;
	LockStack<int>						*session_blank_index_stack_;
	Iocp								*iocp_;

};
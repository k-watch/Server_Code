#pragma once

class Iocp;

//***********************************************
// SERVER_PARAM:			���� ����� �ʿ��� ����
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
// ���� ��� ���
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
	// ShutDown:				���� ���� Shutdown Send ����
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
	// Recv:					���ú� ť ������ Ȯ�� ��
	//							WSARECV ���
	//***********************************************
	bool						RecvComplete(Session *session, int transfer);
	//***********************************************
	// RecvDataProc:			���ú� ť ������ ����
	//***********************************************
	void						RecvDataProc(Session *session);
	////***********************************************
	// SendComplete:			WSASEND �Ϸ� �� ��Ŷ ó��
	//***********************************************
	bool						SendComplete(Session* session, int transfer);
	//***********************************************

	//***********************************************
	// Send Thread Funtion
	//***********************************************
	void						SendThread(void);
	//***********************************************

	//***********************************************
	// TpsUpdateThread:			1�� �� Accept, Recv, Send �� üũ
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
	// OnAuthClientJoin:		���� AUTH_MODE Ŭ���̾�Ʈ ó��
	//***********************************************
	virtual void				OnAuthClientJoin(Session *session)						= 0;
	//***********************************************
	// OnAuthPacketRecv:		AUTH_MODE ���� ���ú� ��Ŷ ó��
	//***********************************************
	virtual void				OnAuthPacketRecv(Session *session, PacketMP *packet)	= 0;
	//***********************************************
	// OnAuthPacketSend:		AUTH_MODE ���� ���� ��Ŷ ó��
	//***********************************************
	virtual void				OnAuthPacketSend(Session *session, PacketMP *packet)	= 0;
	//***********************************************
	// OnGameUpdate:			���� �� ������Ʈ ó��
	//***********************************************
	virtual void				OnGameUpdate(void)										= 0;
	//***********************************************
	// OnAuthClientJoin:		MODE_AUTH_TO_GAME Ŭ���̾�Ʈ ó��
	//***********************************************
	virtual void				OnGameClientJoin(Session *session)						= 0;
	//***********************************************
	// OnGamePacketRecv:		GAME_MODE ���� ���ú� ��Ŷ ó��
	//***********************************************
	virtual void				OnGamePacketRecv(Session *session, PacketMP *packet)	= 0;
	//***********************************************
	// OnGamePacketSend:		GAME_MODE ���� ���� ��Ŷ ó��
	//***********************************************
	virtual void				OnGamePacketSend(Session *session, PacketMP *packet)	= 0;
	//***********************************************
	// OnGameLogout:			���� �α׾ƿ� ��Ŷ ó��
	//***********************************************
	virtual void				OnGameLogout(Session *session)							= 0;
	//***********************************************
	// OnSendUnicast:			�ش� ���� Send ȣ��
	//***********************************************
	virtual void				OnSendUnicast(Session *session, PacketMP *packet)		= 0;
	//***********************************************
	// OnRecv:					��Ŷ ���ú� �� �̺�Ʈ
	//***********************************************
	virtual void				OnRecv(const UPDATE_PARAM &update_param) = 0;
	//***********************************************
	// OnHeartbeat:				��Ʈ��Ʈ ����
	//***********************************************
	virtual void				OnHeartbeat(const THREAD_TYPE &thread_type) = 0;
	// ***********************************************
	// OnStop:					���� ���� ���� �̺�Ʈ
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
	// session_arr_:			���� ���� �迭
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
	// server_exit:				���� ���� �÷���
	//***********************************************
	bool						server_exit_;

	//***********************************************
	// accept_que_:				Accept �Ϸ�� ���� ���� ť
	// session_connect_pool_:	���� Accept �� ���� ���� ����
	// blank_index_stack_:		���� �迭 �ε��� ���� ����
	//***********************************************
	LockQueue<SESSION_CONNECT_PARAM*> 	accept_que_;
	LockStack<SESSION_CONNECT_PARAM>	session_connect_pool_;
	LockStack<int>						*session_blank_index_stack_;
	Iocp								*iocp_;

};
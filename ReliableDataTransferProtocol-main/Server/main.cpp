#include <utility>
#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <netdb.h>
#include <string>
#include <thread>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <chrono>
#include <bits/stdc++.h>

using namespace std;

#define maxSegSize  508
#define AckPacketSize  8
#define ChunkSize 499

enum FSMState {fastRecovery, slowStart, congestionAvoidance};

struct packet 
{
    uint16_t checkSum;
    uint16_t len;
    uint32_t seqNo;
    char data [500];
};

struct packetNotSent
{
    int seqNo;
    bool isFinished;
    chrono::time_point<chrono::system_clock> timer;
};

struct packetAck 
{
    uint16_t checkSum;
    uint16_t len;
    uint32_t ackno;
};

int port, randomSeed;
double plp;
vector<packetNotSent> packetNotSents;
vector<packet> packetsSent;


void handle_client_request(int serverSocket, int client_fd, sockaddr_in client_addr, char rec_buffer [] , int bufferSize);
void sendTheData_HandleCongesion (int client_fd, struct sockaddr_in client_addr , vector<string> data);
bool send_packet(int client_fd, struct sockaddr_in client_addr , string temp_packet_string, int seqNum);

vector<string> readFileData(string fName)
{
    string temp = "";
    vector<string> dataPackets;
    ifstream ifStream;
    ifStream.open(fName);
    if (ifStream)
    {
        char c;
        int idx = 0;
        while(ifStream.get(c))
        {
            if(idx < ChunkSize)
            {
                temp += c;
            }
            else
            {
                dataPackets.push_back(temp);
                temp.clear();
                temp += c;
                idx = 0;
                continue;
            }
            idx++;
        }
        if (idx > 0)
        {
            dataPackets.push_back(temp);
        }
    }
    ifStream.close();
    return dataPackets;

}
uint16_t getAckChecksum (uint16_t len , uint32_t ackNo)
{
    uint32_t sum = 0;
    sum += len;
    sum += ackNo;
    while (sum >> 16)
    {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    uint16_t Sum = (uint16_t) (~sum);
    return Sum;
}
uint16_t getDataChecksum (string content, uint16_t len , uint32_t seqNo)
{
    uint32_t sum = 0;
    sum += len;
    sum += seqNo;
    char a[content.length() + 1];
    strcpy(a, content.c_str());
    for (int i = 0; i < content.length(); i++)
    {
        sum += a[i];
    }
    while (sum >> 16)
    {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (uint16_t) (~sum);
}
packet createPacket(string packetStr, int seqNo) 
{
    struct packet p;
    memset(p.data,0,500);
    strcpy(p.data, packetStr.c_str());
    p.seqNo = seqNo;
    p.len = packetStr.size();
    p.checkSum = getDataChecksum(packetStr, p.len, p.seqNo);
    return p;
}

bool corruptDatagram()
{
    double isLost = (rand() % 100) * plp;
    cout << "Lost val : " << isLost << endl;
    if (isLost >= 5.9)
    {
        return true;
    }
    return false;
}

vector<string> readReq()
{
    string fName = "requests.txt";
    vector<string> reqs;
    string line;
    ifstream f;
    f.open(fName);
    while(getline(f, line))
    {
        reqs.push_back(line);
    }
    return reqs;
}
long checkFileExistence(string fName)
{
     ifstream file(fName.c_str(), ifstream::ate | ifstream::binary);
     if (!file.is_open()) 
     {
        cout << "File opening failure" << endl;
        return -1;
     }
     cout << "Opened successfully" << endl << flush;
     long len = file.tellg();
     file.close();
     return len;
}

int main()
{
    vector<string> args = readReq();
    int portNo = stoi(args[0]);
    randomSeed = stoi(args[1]);
    srand(randomSeed);
    plp = stod(args[2]);
    int serverSocket, clientSocket;
    struct sockaddr_in serverAddress, clientAddress;
    int server_addrlen = sizeof(serverAddress);
    if ((serverSocket = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0)
    {
        cout << "Creating server socket failure" << endl;
        return 1;
    }
    memset(&serverAddress, 0, sizeof(serverAddress));
    memset(&clientAddress, 0, sizeof(clientAddress));
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(portNo);
    serverAddress.sin_addr.s_addr = INADDR_ANY;
    memset(&(serverAddress.sin_zero), '\0', AckPacketSize);
    if (bind(serverSocket, (struct sockaddr *) &serverAddress, sizeof(serverAddress)) < 0)
    {
        cout << "Binding server failure" << endl;
        return 2;
    }
    while (true)
    {
        socklen_t clientAddressLength = sizeof(struct sockaddr);
        cout << "Ready for Connection:" << endl;
        char rec_buffer[maxSegSize];
        ssize_t receivedBytes = recvfrom(serverSocket, rec_buffer, maxSegSize, 0, (struct sockaddr*)&clientAddress, &clientAddressLength);
        if (receivedBytes <= 0)
        {
             cout << "Receiving file bytes failure" << endl;
             return 3;
        }
        // fork child procees to handle the request 
        pid_t pid = fork();
        if (pid == -1)
        {
            cout << "Forking child process for the client failure" << endl;
            return 4;
        }
        else if (pid == 0)
        {
            if ((clientSocket = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0)
            {
                cout << "Creating client socket failure" << endl;
                return 5;
            }
             handle_client_request(serverSocket,clientSocket, clientAddress, rec_buffer , maxSegSize);
             return 6;
         }
    }
    close(serverSocket);
    return 0;
}

void handle_client_request(int serverSocket, int client_fd, struct sockaddr_in client_addr, char rec_buffer [] , int bufferSize) {

     auto* data_packet = (struct packet*) rec_buffer;
     string fName = string(data_packet->data);
     cout << "requested file name from client  : " << fName <<"\n" << " , Lenght : " << fName.size() << endl;
     int fileSize = checkFileExistence(fName);
     if (fileSize == -1){
        return;
     }
     int numberOfPackets = ceil(fileSize * 1.0 / ChunkSize);
     cout << "File Size : " << fileSize << " Bytes , Num. of chuncks : " << numberOfPackets << endl << flush;

     /** send ack to file name **/
     struct packetAck ack;
     ack.checkSum = 0;
     ack.len = numberOfPackets;
     ack.ackno = 0;
     char* buf = new char[maxSegSize];
     memset(buf, 0, maxSegSize);
     memcpy(buf, &ack, sizeof(ack));
     ssize_t bytesSent = sendto(client_fd, buf, maxSegSize, 0, (struct sockaddr *)&client_addr, sizeof(struct sockaddr));
     if (bytesSent == -1) {
        perror("Error Sending The Ack ! ");
        exit(1);
     } else {
        cout << "Ack of file name is sent successfully" << endl << flush;
     }

     /** read data from file **/
     vector<string> dataPackets = readFileData(fName);
     if (dataPackets.size() == numberOfPackets){
        cout << "File Data is read successfully " << endl << flush;
     }

     /** start sending data and handling congestion control using the SM **/
     sendTheData_HandleCongesion(client_fd, client_addr, dataPackets);

}

void sendTheData_HandleCongesion (int client_fd, struct sockaddr_in client_addr , vector<string> data){
    ofstream myFile_Handler;
    // File Open
    myFile_Handler.open("File_1.txt");

    int cwnd_base = 0;
    double cwnd = 1;
    //cout << "CWND : " << cwnd << " maxSegSize "<< endl << flush;
    // Write to the file
    myFile_Handler << cwnd << endl;
    int base_packet_number = 0;
    long sentBytes = 0;
    int sst = 128;
    bool flag = true;
    int seqNum = 0;
    long sentPacketsNotAcked = 0;
    FSMState st = slowStart;
    long numberOfDupAcks = 0;
    int lastAckedSeqNum = -1;
    bool stillExistAcks = true;
    char rec_buf[maxSegSize];
    socklen_t clientAddressLength = sizeof(struct sockaddr);
    int totalPackets = data.size();
    int alreadySentPackets = 0;

    while (flag){

        /**
        this part will run first to send first datagram as stated in pdf.
        **/
        while(cwnd_base < cwnd && alreadySentPackets + packetNotSents.size() < totalPackets){
            seqNum = base_packet_number + cwnd_base;
            string temp_packet_string = data[seqNum];
            /**
                in case error simulated won't send the packet so the seqnumber will not correct at the receiver so will send duplicate ack.
            **/
            bool isSent = send_packet(client_fd, client_addr, temp_packet_string,seqNum);
            if (isSent == false) {
                perror("Error sending data packet ! ");
                //exit(1);
            } else {
                sentPacketsNotAcked++;
                alreadySentPackets++;
                cout << "Sent Seq Num : " << seqNum << endl << flush;
            }
            cwnd_base++;
        }
	    //cout << "///////////////////////////////////"<< "CWND : " << cwnd << " maxSegSize "<< endl << flush;
	    //cout << "///////////////////////////////////"<< " base : " << cwnd_base << endl << flush;

        /*** receiving ACKs ***/
        if (sentPacketsNotAcked > 0){
            stillExistAcks = true;
            while (stillExistAcks){
                cout << "waiting acwnd_baseck " << endl << flush;
                ssize_t receivedBytes = recvfrom(client_fd, rec_buf, AckPacketSize, 0, (struct sockaddr*)&client_addr, &clientAddressLength);
                if (receivedBytes < 0){
                     perror("error receiving bytes ! ");
                     exit(1);
                }
                else if (receivedBytes != AckPacketSize){
                     cout << "Expecting Ack Got Something Else" << endl << flush;
                     exit(1);
                }
                else {

                    auto ack = (packetAck*) malloc(sizeof(packetAck));
                    memcpy(ack, rec_buf, AckPacketSize);
                    cout << "Ack. " << ack->ackno << " Received." << endl << flush;

                    if (getAckChecksum(ack->len, ack->ackno) != ack->checkSum){
                        cout << "Corrupt Ack. received" << endl << flush ;
                    }

                    int ack_seqNo = ack->ackno;
                    if (lastAckedSeqNum == ack_seqNo){

                        numberOfDupAcks++;
                        sentPacketsNotAcked--;
                        if (st == fastRecovery){
                            cwnd++;
                            //cout << "CWND : " << cwnd << " maxSegSize "<< endl << flush;
                            // Write to the file
                            myFile_Handler << cwnd << endl;
                        } else if (numberOfDupAcks == 3){
                            sst = cwnd / 2;
                            cwnd = sst + 3;
                            cout << "///////////////////////////////////////////////////////////////// Triple duplicate Ack /////////////////////////////////////////////////////////////////////////" << endl;
                            //cout << "CWND : " << cwnd << " maxSegSize "<< endl << flush;
                            // Write to the file
                            myFile_Handler << cwnd << endl;
                            st = fastRecovery;
                            /** retransmit the lost packet **/
                            seqNum = ack_seqNo;
                            bool found = false;
                            for (int j = 0; j < packetNotSents.size() ;j++){
                                packetNotSent nspkt = packetNotSents[j];
                                if (nspkt.seqNo == seqNum){
                                      found = true;
                                      string temp_packet_string = data[seqNum];
                                      struct packet data_packet = createPacket(temp_packet_string, seqNum);
                                      char sendBuffer [maxSegSize];
                                      memset(sendBuffer, 0, maxSegSize);
                                      memcpy(sendBuffer, &data_packet, sizeof(data_packet));
                                      ssize_t bytesSent = sendto(client_fd, sendBuffer, maxSegSize, 0, (struct sockaddr *)&client_addr, sizeof(struct sockaddr));
                                      if (bytesSent == -1) {
                                        //bool isSent = send_packet(client_fd, client_addr, temp_packet_string,seqNum);
                                        //if (isSent == false) {
                                          perror("error re-sending data packet ! ");
                                          exit(1);
                                      } else {
                                          sentPacketsNotAcked++;
                                          alreadySentPackets++;
                                          packetNotSents.erase(packetNotSents.begin() + j);
                                      }
                                      break;
                                }
                            }

                            /** handle checksum error **/
                            if (!found){
                                for (int j = 0; j < packetsSent.size() ;j++){
                                    packet spkt = packetsSent[j];
                                    if (spkt.seqNo == seqNum){
                                          found = true;
                                          string temp_packet_string = data[seqNum];
                                          struct packet data_packet = createPacket(temp_packet_string, seqNum);
                                          char sendBuffer [maxSegSize];
                                          memset(sendBuffer, 0, maxSegSize);
                                          memcpy(sendBuffer, &data_packet, sizeof(data_packet));
                                          ssize_t bytesSent = sendto(client_fd, sendBuffer, maxSegSize, 0, (struct sockaddr *)&client_addr, sizeof(struct sockaddr));
                                          if (bytesSent == -1) {
                                          //bool isSent = send_packet(client_fd, client_addr, temp_packet_string,seqNum);
                                          //if (isSent == false) {
                                              perror("error re-sending data packet ! ");
                                              exit(1);
                                          } else {
                                              alreadySentPackets++;
                                              packetsSent.erase(packetsSent.begin() + j);
                                          }
                                          break;
                                    }
                                }

                            }

                        }
                        //cout << "CWND : " << cwnd << " maxSegSize "<< endl << flush;

                    } else if (lastAckedSeqNum < ack_seqNo) {
                        /** new ack : compute new base and packet no. and handling congestion control FSM **/
                        cout << "newAck " << endl;
                        numberOfDupAcks = 0;
                        lastAckedSeqNum = ack_seqNo;
                        int advance = lastAckedSeqNum - base_packet_number;
                        cwnd_base = cwnd_base - advance;
                        base_packet_number = lastAckedSeqNum;
                        if (st == slowStart){
                           if (cwnd*2 >= sst){
                                st = congestionAvoidance;
                                cwnd++;
                           }
                           else{
                               cwnd=cwnd*2;
                           }
                           //cout << "CWND : " << cwnd << " maxSegSize "<< endl << flush;
                           // Write to the file
                           myFile_Handler << cwnd << endl;
                           if (cwnd >= sst){
                                st = congestionAvoidance;
                           }
                        } else if (st == congestionAvoidance){
                            cwnd ++;
                            ///cwnd += (1 / floor(cwnd));
                            //cout << "CWND : " << cwnd << " maxSegSize "<< endl << flush;
                            // Write to the file
                            myFile_Handler << cwnd << endl;
                        } else if (st == fastRecovery){
                            st = congestionAvoidance;
                            cwnd = sst;
                            //cout << "CWND : " << cwnd << " maxSegSize "<< endl << flush;
                            // Write to the file
                            myFile_Handler << cwnd << endl;
                        }
                        //cout << "CWND : " << cwnd << " maxSegSize "<< endl << flush;
                        //cout << " base : " << cwnd_base << endl << flush;
                        //cout << " packet : " << base_packet_number << endl << flush;
                        sentPacketsNotAcked--;
                    } else {
                        sentPacketsNotAcked--;
                        //cout << "CWND : " << cwnd << " maxSegSize "<< endl << flush;
                    }

                    if (sentPacketsNotAcked == 0){
                        //cout << "CWND : " << cwnd << " maxSegSize "<< endl << flush;
                        stillExistAcks = false;
                    }

                }

            }

        }


        /** Handle Time Out **/
        bool entered=false;
        for (int j = 0; j < packetNotSents.size() ;j++){
            packetNotSent nspkt = packetNotSents[j];
            chrono::time_point<chrono::system_clock> current_time = chrono::system_clock::now();
            chrono::duration<double> elapsed_time = current_time - nspkt.timer;
            if (elapsed_time.count() >= 2){
                 entered=true;

                //  cwnd=1;
                //  //cwnd_base=0;
                //  sst=128;
                //  st = slowStart;
                //  //cout << "CWND : " << cwnd << " maxSegSize "<< endl << flush;
                //  // Write to the file
                //  myFile_Handler << cwnd << endl;
                
                 cout << "Timed Out ! " << endl << flush;
                 cout << "Re-transmitting the packet " << endl << flush;
                 seqNum = nspkt.seqNo;
                 string temp_packet_string = data[seqNum];
                 struct packet data_packet = createPacket(temp_packet_string, seqNum);
                 char sendBuffer [maxSegSize];
                 memset(sendBuffer, 0, maxSegSize);
                 memcpy(sendBuffer, &data_packet, sizeof(data_packet));
                 ssize_t bytesSent = sendto(client_fd, sendBuffer, maxSegSize, 0, (struct sockaddr *)&client_addr, sizeof(struct sockaddr));
                 if (bytesSent == -1) {
                        perror("error resending the data packet ! ");
                        exit(1);
                 } else {
                        sentPacketsNotAcked++;
                        alreadySentPackets++;
                        packetNotSents.erase(packetNotSents.begin() + j);
                        j--;
                        cout << "Sent Seq Num : " << seqNum << endl << flush;
                 }
            }
        }
        if(entered){
            entered=false;
            cwnd=1;
            //cwnd_base=0;
            st = slowStart;
            //cout << "CWND : " << cwnd << " maxSegSize "<< endl << flush;
            // Write to the file
            myFile_Handler << cwnd << endl;
        }

    }

    // File Close
    myFile_Handler.close();
}

bool send_packet(int client_fd, struct sockaddr_in client_addr , string temp_packet_string, int seqNum){
     char sendBuffer [maxSegSize];
     struct packet data_packet = createPacket(temp_packet_string, seqNum);
     bool corrupt=corruptDatagram();
     if(corrupt){
         data_packet.checkSum=data_packet.checkSum-1;
     }
     memset(sendBuffer, 0, maxSegSize);
     memcpy(sendBuffer, &data_packet, sizeof(data_packet));
     //cout << data_packet.data << endl;
     if (!corruptDatagram()&&!corrupt){
         ssize_t bytesSent = sendto(client_fd, sendBuffer, maxSegSize, 0, (struct sockaddr *)&client_addr, sizeof(struct sockaddr));
         if (bytesSent == -1) {
                return false;
         } else {
                packetsSent.push_back(data_packet);
                return true;
         }
     } else {
        cout << "///////////////////////////////////Drop data" << endl; 
        struct packetNotSent nspacket;
        nspacket.seqNo = seqNum;
        nspacket.isFinished = false;
        nspacket.timer = chrono::system_clock::now();
        packetNotSents.push_back(nspacket);

        return false;
     }
}



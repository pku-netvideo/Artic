recv_log_dir = "sim_test/sim_receiver"
recv_log_name = "logs_recv.txt"
send_log_dir = "sim_test/sim_sender"
send_log_name = "logs_send.txt"

FPS = 30

import os

def load_data_from(dir, name):
    filepath = os.path.join(dir, name)
    result = []
    keys_order = []
    
    with open(filepath, 'r', encoding='utf-8') as f:
        lines = f.readlines()
        
    if not lines:
        raise Exception("Short data length!!")
    if len(lines) < 10:
        raise Exception("Short data length!!")
    lines = lines[:-1] # 去除没输出完的行和多余空行
    
    # 处理每一行数据
    for line_num, line in enumerate(lines):
        line = line.strip()
        if not line:
            continue
            
        # 分割键值对
        pairs = line.split(',')
        row_dict = {}
        
        for pair in pairs:
            if ':' not in pair:
                continue
                
            key, value = pair.split(':', 1)
            # 处理key：去除两端空格，将中间空格替换为下划线
            key = key.strip().replace(' ', '_')
            # 处理value：去除两端空格，转换为整数
            value = value.strip()
            try:
                # 尝试转换为整数
                row_dict[key] = int(value)
            except ValueError:
                # 如果不是整数，保持字符串
                row_dict[key] = value
        
        # 如果是第一行，记录键的顺序
        if line_num == 0:
            keys_order = list(row_dict.keys())
            
        result.append(row_dict)
    
    # 输出key列表（用逗号隔开）
    print("Keys:", ','.join(keys_order))
    print("Size:", len(result))
    
    return result

print("Load recv data:")
recv_data = load_data_from(recv_log_dir, recv_log_name)
print("Load send data:")
send_data = load_data_from(send_log_dir, send_log_name)

print()
print("Send:", len(send_data), ";  Recv:", len(recv_data))

def analyze(send_data, recv_data):
    """
    分析发送端和接收端数据，生成每帧的分析结果
    
    参数:
    send_data: list of dict, 发送端数据
    recv_data: list of dict, 接收端数据
    
    返回:
    list of dict, 每帧的分析结果，包含FrameId, bitrate, drop, delay
    """
    # 创建接收端数据的索引字典，以FrameId为键
    recv_dict = {}
    for recv_frame in recv_data:
        frame_id = recv_frame.get('FrameId')
        if frame_id is not None:
            recv_dict[frame_id] = recv_frame
    
    # 结果列表
    result = []
    
    # 遍历发送端数据
    for send_frame in send_data:
        frame_id = send_frame.get('FrameId')
        bitrate = send_frame.get('bitrate')
        bitrate_bound = send_frame.get('bitrate_bound')
        frame_size = send_frame.get('frame_size')
        
        if frame_id is None or bitrate is None:
            # 跳过无效数据
            continue
        
        # 检查是否丢帧
        if frame_id not in recv_dict:
            # 丢帧
            result.append({
                "FrameId": frame_id,
                "bitrate_bbr": bitrate,
                "bitrate_bound": bitrate_bound,
                "bitrate": int(frame_size * FPS * 0.008),
                "drop": 1,
                "delay": -send_frame.get('Timestamp') # 未来计算和下一接受帧的差
            })
        else:
            # 成功接收
            recv_frame = recv_dict[frame_id]
            delay = recv_frame.get('OWD')
            
            result.append({
                "FrameId": frame_id,
                "bitrate_bbr": bitrate,
                "bitrate_bound": bitrate_bound,
                "bitrate": int(frame_size * FPS * 0.008),
                "drop": 0,
                "delay": delay
            })
    
    # 按FrameId排序结果
    result.sort(key=lambda x: x["FrameId"])
    
    next_recv_time = None
    L = None
    for i in reversed(range(len(result))):
        frame_id = result[i]["FrameId"]
        if frame_id in recv_dict:
            next_recv_time = recv_dict[frame_id].get('Timestamp')
            if L is None:
                L = i + 1
            
        if result[i]["drop"] == 1:
            if next_recv_time is not None:
                result[i]["delay"] += next_recv_time
            else:
                result[i]["delay"] = -1
    
    print(L, "frames analyzed.")
    avg_delay = sum(x["delay"] for x in result[:L]) / L
    avg_bitrate = sum(x["bitrate"] for x in result[:L]) / L
    print("avg delay = {:.4f};  avg bitrate = {:.1f} kbps".format(avg_delay, avg_bitrate))
    return {
        "avg_delay": avg_delay,
        "avg_bitrate": avg_bitrate,
        "pkts": result[:L],
        "sender_data": send_data,
        "receiver_data": recv_data,
    }


import json
result = analyze(send_data, recv_data)
print("Average delay:", result["avg_delay"])
print("Average bitrate:", result["avg_bitrate"])

with open("result.json","w") as file_all_logs:
    json.dump(result, file_all_logs)
with open("pkts.jsonl","w") as file_pkt:
    for json_piece in result["pkts"]:
        json_line = json.dumps(json_piece)
        file_pkt.write(json_line + "\n")
#!/usr/bin/env python3
"""Capture a future native XNU CDC console directly. No ADB or device writes."""
import argparse,datetime,hashlib,importlib.util,json,os,plistlib,select,signal,socket,subprocess,sys,termios,time,tty
from pathlib import Path

def load_control_codec():
 spec=importlib.util.spec_from_file_location('xnu_usb_control_host',Path(__file__).with_name('xnu_usb_control.py'))
 module=importlib.util.module_from_spec(spec);sys.modules[spec.name]=module;spec.loader.exec_module(module);return module

control=load_control_codec()

def walk(n):
 if isinstance(n,dict):
  yield n
  for c in n.get('IORegistryEntryChildren',[]):yield from walk(c)
 elif isinstance(n,list):
  for c in n:yield from walk(c)

class CaptureStopped(Exception):pass

def find(serial,wait,observations=None,should_stop=None):
 end=time.monotonic()+wait
 while time.monotonic()<end:
  if should_stop and should_stop():raise CaptureStopped()
  q=subprocess.run(['ioreg','-a','-l','-r','-c','IOUSBHostDevice'],capture_output=True,timeout=10);devices=[]
  if q.returncode==0 and q.stdout.strip():
   try:devices=plistlib.loads(q.stdout)
   except plistlib.InvalidFileException:pass
  for n in walk(devices):
   if n.get('USB Serial Number')==serial or (n.get('idVendor')==0x1209 and n.get('idProduct')==0x316d):
    state={k:n.get(k) for k in ['USB Product Name','USB Serial Number','idVendor','idProduct','IORegistryEntryName']}
    state['ports']=sorted(set(c['IOCalloutDevice'] for c in walk(n) if c.get('IOCalloutDevice','').startswith('/dev/cu.usbmodem')))
    if observations is not None and (not observations or observations[-1]['device']!=state):
     observations.append({'observed_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'device':state})
   if n.get('USB Serial Number')!=serial:continue
   paths=sorted(set(c['IOCalloutDevice'] for c in walk(n) if c.get('IOCalloutDevice','').startswith('/dev/cu.usbmodem')))
   if len(paths)==1:return paths[0]
   if len(paths)>1:raise RuntimeError('Multiple CDC interfaces; select the log interface after driver descriptors are finalized')
  time.sleep(1)
 raise RuntimeError('Expected Qualcomm CDC serial has not attached')

def nonnegative(value):
 value=float(value)
 if value < 0 or not value < float('inf'):raise argparse.ArgumentTypeError('must be finite and nonnegative')
 return value

def byte_limit(value):
 value=int(value)
 if value < 0:raise argparse.ArgumentTypeError('must be nonnegative')
 return value

def atomic_record(path,record):
 temporary=path.with_name(path.name+'.tmp')
 with temporary.open('w') as out:
  json.dump(record,out,indent=2);out.write('\n');out.flush();os.fsync(out.fileno())
 os.replace(temporary,path)

class ControlBridge:
 """One serial reader with a bounded, nonblocking private Unix socket bridge."""
 MAX_CLIENTS=4;REQUEST_TIMEOUT=5.0;RESPONSE_TIMEOUT=5.0
 def __init__(self,path,serial_fd,metadata_path):
  self.path=Path(path);self.serial_fd=serial_fd
  if os.path.lexists(self.path):raise FileExistsError('control socket already exists; refusing to replace it')
  self.listener=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM)
  try:
   self.listener.bind(str(self.path));os.chmod(self.path,0o600)
   self.listener.listen(self.MAX_CLIENTS);self.listener.setblocking(False)
  except BaseException:
   self.listener.close();raise
  self.clients={};self.pending=None;self.serial_out=bytearray();self.serial_deadline=None
  self.metadata_fd=os.open(metadata_path,os.O_WRONLY|os.O_CREAT|os.O_APPEND|os.O_CLOEXEC,0o600)
  self.metadata_error=None
  self.stats=dict(accepted=0,rejected=0,busy=0,forwarded=0,reply_frames=0,delivered=0,timeouts=0,disconnects=0)
 def _metadata(self,event,request=None,reply=None,result=None):
  if self.metadata_fd is None:return
  record=dict(updated_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),event=event)
  if request is not None:
   record.update(session=f'{request.session:016x}',seq=f'{request.seq:08x}',verb=request.verb)
  if reply is not None:
   record.update(reply_status=reply.status,reply_session=f'{reply.session:016x}',reply_seq=f'{reply.seq:08x}',fields=reply.fields)
  if result is not None:record['result']=result
  pending=memoryview((json.dumps(record,sort_keys=True,separators=(',',':'))+'\n').encode())
  try:
   while pending:
    written=os.write(self.metadata_fd,pending)
    if not written:raise OSError('control metadata write made no progress')
    pending=pending[written:]
   os.fsync(self.metadata_fd)
  except OSError as exc:
   self.metadata_error=str(exc)
   try:os.close(self.metadata_fd)
   except OSError:pass
   self.metadata_fd=None
 def _drop(self,client,result=None,reply=None):
  if client.get('closed'):return
  client['closed']=True
  descriptor=client['socket'].fileno()
  if result is not None:
   self._metadata('command-result',client.get('request'),reply,result)
  if self.pending is client:
   self.pending=None;self.serial_out.clear();self.serial_deadline=None
  try:client['socket'].close()
  except OSError:pass
  self.clients.pop(descriptor,None)
 def _accept(self):
  while True:
   try:connection,_=self.listener.accept()
   except BlockingIOError:return
   if len(self.clients)>=self.MAX_CLIENTS:
    connection.close();self.stats['busy']+=1;continue
   connection.setblocking(False)
   self.clients[connection.fileno()]=dict(socket=connection,input=bytearray(),request=None,response=None,reply=None,deadline=time.monotonic()+self.REQUEST_TIMEOUT)
   self.stats['accepted']+=1
 def read_fds(self):return [self.listener,*[c['socket'] for c in self.clients.values() if not c.get('closed')]]
 def write_fds(self):return [c['socket'] for c in self.clients.values() if c.get('response') is not None and not c.get('closed')]
 def wants_serial_write(self):return bool(self.serial_out)
 def _read_client(self,client):
  connection=client['socket']
  if client['request'] is not None or client['response'] is not None:
   try:extra=connection.recv(1)
   except BlockingIOError:return
   except OSError:self._drop(client,'socket-read-error');return
   if extra:self._drop(client,'trailing-input');return
   self.stats['disconnects']+=1;self._drop(client,'client-disconnected');return
  try:data=connection.recv(control.FRAME_BYTES+1-len(client['input']))
  except BlockingIOError:return
  except OSError:self._drop(client,'socket-read-error');return
  if not data:
   self.stats['disconnects']+=1;self._drop(client,'client-disconnected');return
  client['input'].extend(data)
  if len(client['input'])>control.FRAME_BYTES:
   self.stats['rejected']+=1;self._drop(client,'rejected-malformed');return
  newline=client['input'].find(b'\n')
  if newline<0:return
  if newline!=len(client['input'])-1:
   self.stats['rejected']+=1;self._drop(client,'rejected-trailing-bytes');return
  try:request=control.parse_request(bytes(client['input']))
  except control.ControlError:
   self.stats['rejected']+=1;self._drop(client,'rejected-malformed');return
  if self.pending is not None or self.serial_out:
   self.stats['busy']+=1;self._metadata('command-result',request,result='busy');self._drop(client);return
  client['request']=request;client['deadline']=time.monotonic()+self.REQUEST_TIMEOUT
  self.pending=client;self.serial_out.extend(request.raw);self.serial_deadline=client['deadline']
  self.stats['forwarded']+=1;self._metadata('command',request,result='forwarded')
 def _read_ready(self,ready):
  for item in ready:
   if item is self.listener:self._accept();continue
   client=self.clients.get(item.fileno())
   if client is not None:self._read_client(client)
 def _flush_serial(self):
  if not self.serial_out:return
  if time.monotonic()>=self.serial_deadline:
   self.stats['timeouts']+=1;self._drop(self.pending,'serial-write-timeout');return
  try:written=os.write(self.serial_fd,self.serial_out)
  except BlockingIOError:return
  except OSError:self._drop(self.pending,'serial-write-error');return
  if not written:
   self._drop(self.pending,'serial-write-error');return
  del self.serial_out[:written]
  if not self.serial_out:self.serial_deadline=None
 def _matches(self,request,reply):
  if reply.verb!=request.verb:return False
  if request.verb=='HELLO':return reply.seq==0
  return reply.session==request.session and reply.seq==request.seq
 def handle_replies(self,replies):
  for reply in replies:
   self.stats['reply_frames']+=1
   client=self.pending
   if client is None:continue
   request=client.get('request')
   if request is None or not self._matches(request,reply):continue
   client['response']=bytearray(reply.raw);client['reply']=reply
   client['deadline']=time.monotonic()+self.RESPONSE_TIMEOUT;self.pending=None
   self._metadata('reply',request,reply,result='ok' if reply.status=='OK' else 'error')
 def _write_client(self,client):
  response=client.get('response')
  if response is None:return
  try:written=client['socket'].send(response)
  except BlockingIOError:return
  except OSError:self._drop(client,'socket-write-error');return
  if not written:self._drop(client,'socket-write-error');return
  del response[:written]
  if not response:
   self.stats['delivered']+=1;self._drop(client)
 def _write_ready(self,ready):
  for item in ready:
   client=self.clients.get(item.fileno())
   if client is not None:self._write_client(client)
 def service(self,read_ready,write_ready,serial_writable):
  if serial_writable:self._flush_serial()
  self._read_ready(read_ready);self._write_ready(write_ready)
  now=time.monotonic()
  for client in list(self.clients.values()):
   if client.get('closed') or now<client['deadline']:continue
   self.stats['timeouts']+=1
   self._drop(client,'reply-timeout' if client.get('request') else 'socket-timeout')
 def close(self):
  for client in list(self.clients.values()):self._drop(client,'capture-ended')
  try:self.listener.close()
  except OSError:pass
  self.serial_out.clear();self.pending=None
  if self.metadata_fd is not None:
   try:os.close(self.metadata_fd)
   except OSError:pass
   self.metadata_fd=None
  return dict(path=str(self.path),metadata_error=self.metadata_error,**self.stats)

def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--serial',default='QcomXnuUsbDebug');p.add_argument('--output',type=Path,required=True)
 p.add_argument('--wait',type=nonnegative,default=120)
 p.add_argument('--seconds',type=nonnegative,default=120,help='capture duration after attach; 0 runs until disconnect, marker or signal')
 p.add_argument('--max-bytes',type=byte_limit,default=0,help='optional total byte ceiling; 0 (default) has no size ceiling')
 p.add_argument('--sync-seconds',type=nonnegative,default=1,help='fsync/checkpoint interval; 0 syncs each read')
 p.add_argument('--stage',choices=['xnu','uefi-usb-probe'],default='xnu')
 p.add_argument('--until-marker',default='QCOM PANIC END\n')
 p.add_argument('--no-marker',action='store_true',help='continue after panic markers until duration, disconnect or signal')
 p.add_argument('--verify-logs',action='store_true',help='after closing capture, decode and require early/kernel/current user snapshot coverage')
 p.add_argument('--control-socket',action='store_true',help='create a private Unix socket for bounded hello/ping/status/reboot/screenshot frames')
 a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
 reserved=('xnu-usb.log','capture.json','capture-progress.json')
 if a.verify_logs:reserved+=('logs',)
 if a.control_socket:reserved+=('usb-control.sock','usb-control.jsonl')
 if any(os.path.lexists(a.output/name) for name in reserved):
  p.error('capture output already exists; choose a fresh output directory')
 start=datetime.datetime.now(datetime.timezone.utc).isoformat();size=0;synced=0;sha=hashlib.sha256();status='duration-ended';tail=b'';path=None;fd=None;error=None;observations=[]
 bridge=None;bridge_summary=None;extractor=None;control_path=(a.output/'usb-control.sock' if a.control_socket else None)
 marker=b'' if a.no_marker else a.until_marker.encode()
 stop_requested=False
 def request_stop(signum,frame):
  # Never unwind between FileIO.write and its byte/hash accounting. Signal
  # handlers only request an orderly stop outside that indivisible sequence.
  nonlocal stop_requested
  stop_requested=True
 previous_sigterm=signal.signal(signal.SIGTERM,request_stop)
 previous_sigint=signal.signal(signal.SIGINT,request_stop)
 try:
  path=find(a.serial,a.wait,observations,lambda:stop_requested)
  if stop_requested:raise CaptureStopped()
  fd=os.open(path,os.O_RDWR|os.O_NOCTTY)
  # TCSAFLUSH (setraw's default) discards kernel logs already buffered while
  # IOKit publishes the callout port. Change settings without flushing input.
  tty.setraw(fd,termios.TCSANOW);attrs=termios.tcgetattr(fd);attrs[2]|=termios.CLOCAL|termios.CREAD;termios.tcsetattr(fd,termios.TCSANOW,attrs)
  if a.control_socket:
   os.set_blocking(fd,False)
   bridge=ControlBridge(control_path,fd,a.output/'usb-control.jsonl');extractor=control.ReplyExtractor()
  end=time.monotonic()+a.seconds if a.seconds else None
  last_sync=time.monotonic()
  with (a.output/'xnu-usb.log').open('xb',buffering=0) as out:
   def checkpoint():
    nonlocal synced,last_sync
    os.fsync(out.fileno());synced=size;last_sync=time.monotonic()
    atomic_record(a.output/'capture-progress.json',dict(updated_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),port=path,synced_bytes=synced,sha256=sha.hexdigest(),status='capturing'))
   try:
    while not stop_requested and (end is None or time.monotonic()<end):
     timeout=1 if end is None else min(1,max(0,end-time.monotonic()))
     read_fds=[fd]+(bridge.read_fds() if bridge is not None else [])
     write_fds=(([fd] if bridge.wants_serial_write() else [])+bridge.write_fds()) if bridge is not None else []
     ready_read,ready_write,_=select.select(read_fds,write_fds,[],timeout)
     if fd not in ready_read:
      if size != synced and time.monotonic()-last_sync>=a.sync_seconds:checkpoint()
      if bridge is not None:bridge.service([x for x in ready_read if x is not fd],[x for x in ready_write if x is not fd],fd in ready_write)
      continue
     request=min(65536,a.max_bytes-size) if a.max_bytes else 65536
     try:data=os.read(fd,request)
     except BlockingIOError:data=None
     except OSError:status='usb-disconnected';break
     if data is not None and not data:status='usb-eof';break
     if data is not None:
      # Unbuffered, bounded writes preserve raw bytes without retaining the
      # capture in memory. Account for partial filesystem writes explicitly.
      pending=memoryview(data)
      while pending:
       written=out.write(pending)
       if not written:raise OSError('log write made no progress')
       sha.update(pending[:written]);size+=written;pending=pending[written:]
      combined=tail+data
      found=bool(marker and marker in combined)
      tail=combined[-(len(marker)-1):] if len(marker)>1 else b''
      replies=extractor.feed(data) if extractor is not None else []
      if replies:
       # The untouched reply bytes reach the raw log and stable storage before
       # any validated copy can be delivered to the control socket client.
       checkpoint();bridge.handle_replies(replies);bridge.service([],bridge.write_fds(),False)
     else:found=False
     if bridge is not None:bridge.service([x for x in ready_read if x is not fd],[x for x in ready_write if x is not fd],fd in ready_write)
     if time.monotonic()-last_sync>=a.sync_seconds:checkpoint()
     if found:status='end-marker';break
     if a.max_bytes and size>=a.max_bytes:status='capture-bound-reached';break
    if stop_requested:status='interrupted'
   finally:
    checkpoint()
 except (CaptureStopped,KeyboardInterrupt):
  status='interrupted'
 except (RuntimeError,OSError,subprocess.SubprocessError) as e:
  status='capture-failed';error=str(e)
 finally:
  signal.signal(signal.SIGTERM,previous_sigterm)
  signal.signal(signal.SIGINT,previous_sigint)
  if bridge is not None:bridge_summary=bridge.close()
  if fd is not None:os.close(fd)
 r=dict(status=status,started_utc=start,finished_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),port=path,usb_serial=a.serial,usb_observations=observations,bytes=size,synced_bytes=synced,sha256=sha.hexdigest(),max_bytes=a.max_bytes,duration_seconds=a.seconds,until_marker=marker.decode(),stage=a.stage,transport='USB CDC ACM; Android/ADB not used for capture',error=error,control_socket_requested=a.control_socket,control_socket_path=str(control_path) if control_path is not None else None,control=bridge_summary,partition_writes_issued=False)
 r['logging_verification_requested']=a.verify_logs
 atomic_record(a.output/'capture.json',r)
 if a.verify_logs:
  try:
   from log_capture import verify_capture
   r['logging']=verify_capture(a.output)
  except (OSError,ValueError,RuntimeError) as exc:
   r['logging']=dict(verified=False,error=str(exc))
  atomic_record(a.output/'capture.json',r)
 print(json.dumps(r,indent=2))
 return 0 if size and status!='capture-failed' and (not a.verify_logs or r['logging']['verified']) else 1
if __name__=='__main__':raise SystemExit(main())

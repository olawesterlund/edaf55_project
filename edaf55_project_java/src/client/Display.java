package client;

public class Display extends Thread {
	
	private ClientMonitor clientMonitor;
	private DisplayMonitor displayMonitor;
	private int camNbr;
	private boolean oldSyncMode, synced;
	
	public Display(DisplayMonitor displayMonitor, ClientMonitor clientMonitor, int CAM_NBR) {
		this.displayMonitor = displayMonitor;
		this.clientMonitor = clientMonitor;
		this.camNbr = CAM_NBR;
		oldSyncMode = true;
	}
	
	public void run() {
		while(true) {
			byte[] pic = clientMonitor.getPicture(camNbr);
			if(camNbr == 1) {
				displayMonitor.updatePicture1(pic);
			}
			else {
				displayMonitor.updatePicture2(pic);
			}
			
			synced = clientMonitor.getSyncMode();
			if(synced != oldSyncMode) {
				displayMonitor.setSyncLabel(synced);
				oldSyncMode = synced;
			}
		}
	}
	
}

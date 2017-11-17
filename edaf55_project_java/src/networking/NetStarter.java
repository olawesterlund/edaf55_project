package networking;

public class NetStarter {

	private static class StartClient extends Thread {
		public void run() {
			System.out.println("Starting network client");
			NetMonitor monitor = new NetMonitor();
			Thread[] threads = new Thread[] {
				new PicturePoller(monitor),
				new PictureReciever(monitor),
				new ClientConnectionThread(monitor, "127.0.0.1", 9999),
				new ClientShutdownThread(monitor)
			};
			
			// Start threads
			for (Thread thread : threads) thread.start();
		}
	}

	public static void main(String[] args) {
		try {
			Thread b = new StartClient();
			b.start();
			b.join();
		} catch (InterruptedException e) {
		}
	}
}

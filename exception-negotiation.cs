using System;

public class Test
{
	[System.Runtime.InteropServices.DllImport ("libexception-negotiation-native")]
	private static extern void install_mach_exception_handlers ();

	[System.Runtime.InteropServices.DllImport ("libexception-negotiation-native")]
	private static extern void generate_native_abort ();

	public static int Main (String [] arguments)
	{
		Wrapper wrapper = null;
		install_mach_exception_handlers ();

		// This should generate and catch a NullReferenceException.
		try {
			Console.WriteLine (wrapper.value);
		} catch (NullReferenceException exception) {
			Console.Error.WriteLine (exception);
		}

		// This should generate a crash report.
		try {
			generate_native_abort ();
		} catch (Exception exception) {
			Console.Error.WriteLine (exception);
			return 1;
		}

		return 0;
	}
}

class Wrapper
{
	public int value;

	public Wrapper (int value)
	{
		this.value = value;
	}
}

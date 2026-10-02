/* SPDX-License-Identifier: BSD-3-Clause */
#import "BtrfsDeviceBarrier.h"
#import <Security/Security.h>

NSString *
btrfs_peer_requirement(NSString *identifier)
{
	SecCodeRef code = NULL;
	CFDictionaryRef information = NULL;
	NSString *team = nil;
	NSCharacterSet *invalid;

	if (SecCodeCopySelf(kSecCSDefaultFlags, &code) != errSecSuccess) {
		return nil;
	}
	if (SecCodeCopySigningInformation(code, kSecCSSigningInformation, &information) ==
	    errSecSuccess) {
		team = [((__bridge NSDictionary *)
			information)[(__bridge NSString *)kSecCodeInfoTeamIdentifier] copy];
		CFRelease(information);
	}
	CFRelease(code);
	invalid = [[NSCharacterSet
	    characterSetWithCharactersInString:@"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"]
	    invertedSet];
	if (team.length == 0 || [team rangeOfCharacterFromSet:invalid].location != NSNotFound) {
		return nil;
	}
	return [NSString stringWithFormat:@"anchor apple generic and identifier \"%@\" and "
					  @"certificate leaf[subject.OU] = \"%@\"",
	    identifier, team];
}

#!/usr/bin/env bash

# TODO:
# - For wavenumbers 1900 - 2200 (or alt widest 1880 - 2260):
#   average points assuming noise,
#   then subtract from all values,
#   and output result i.e. evaluate "100 - n" as before.

a=( )
c=( )
# Split command-line parameters into option switches and otherwise e.g. file paths.
for s in "$@"
do
	[[ "${s:0:1}" == "-" ]] && c+=( "$s" ) || a+=( "$s" )
done

for k in "${!a[@]}"
do
	s="${a["$k"]}"
	[[ -d "$s" ]] || continue
	find "$s" -iname '*.asp' -exec "$0" "${c[@]}" {} +
	unset a["$k"]
done

# Unusual choice of delimiter for inline here document.
awk -f /dev/stdin "${c[@]}" -- "${a[@]}" << "#!/usr/bin/env cat"

# Default log to standard error - override with -vlogf=path to awk.
BEGIN {
	# record count
	meta[0]=0
	# file count
	meta[1]=0
	# wavenumbers
	keys[0]=0
	# parallel array
	data[0]=0

	if (logf == "" || quiet == "") logf="/dev/stderr"
	if (dbgf == "" && debug == "") dbgf="/dev/null"

	sameunit+=0
	if (avgf == "-") avgf="/dev/stdout"

	ec=0
}

# Strip carriage return and line feed.
function rmeol(str,	n_)
{
	n_=length(str)
	if (n_ > 0 && substr(str, n_) == "\n") str=substr(str, 1, --n_)
	if (n_ > 0 && substr(str, n_) == "\r") str=substr(str, 1, --n_)
	return str
}

# Local variables end in '_' - awk convention uses extra spacing.
function asp2txt_check(fname, glob, bi, ni, pi, qi,    b_, c_, d_, i_, k_, n_, p_, q_, r_, w_)
{
	d_=6
	c_=0
	n_=0
	p_=0
	q_=0
	b_=0
	k_=0
	i_=0
	w_=0
	while ((rc = getline <fname) > 0) {
		r_=rmeol($0)
		### printf("# Line = '%s'\n", r_) >>dbgf
		# Handle first 3 lines specially then discard next 3.
		c_++
		if (c_ == 1) {
			n_=r_+0
		} else if (c_ == 2) {
			p_=r_+0
		} else if (c_ == 3) {
			q_=r_+0
		}
		if (c_ <= d_) {
			if (n_ == 0 || p_ == 0 || q_ == 0) continue
			if (n_ <= 1) {
				printf("Undersized input data section from '%s'.\n", fname) >>logf
				return 1
			}
			w_=p_
			i_=(q_ - p_) / (n_ - 1)
			continue
		}
		if (c_ > n_ + d_) {
			printf("Too many records from input '%s'.\n", fname) >>logf
			return 1
		}
		r_+=0
#		if (r_ < -0.02 || r_ > 1.02) {
#			printf("Value out of range from input '%s' - got '%s'.\n", fname, r_) >>logf
#			return 1
#		}
		# Alternate range: 1880 - 2260
#		if (w_ >= 1900 && w_ <= 2200) {
#			b_+=100 - r_
#			k_++
#		}
		w_+=i_
	}
	if (rc < 0) {
		printf("Fatal: %s\n", ERRNO) >>logf
		exit
	}
	if (c_ != n_ + d_) {
		printf("Unexpected number of input records from '%s'.\n", fname) >>logf
		return 1
	}
	if (k_ < 0) {
		printf("Bad quotient for average from '%s'.\n", fname) >>logf
		return 1
	}
	if (k_ == 0) {
#		printf("Zero sum: bgnd = %s\n", b_) >>dbgf
	}
	if (k_ > 0) {
		printf("Average: div = %s, bgnd = %s, avg = %s: input from '%s'\n", k_, b_, b_ / k_, fname) >>dbgf
		b_ = b_ / k_
	}

	### printf("# OK!\n") >>dbgf
	glob[bi]=b_
	glob[ni]=n_
	glob[pi]=p_
	glob[qi]=q_
	return 0
}

function asp2txt_convert(aspname, txtname, avgf, meta, keys, data, b, n, p, q,    a_, c_, d_, f_, i_, r_, t_, v_, w_)
{
	if (p < 0 || p > 10^9 || q < 0 || p > 10^9) {
		printf("Invalid wave number range from '%s'.\n", aspname) >>logf
		return 1
	}
	if (n <= 1) {
		printf("Empty input data section from '%s'.\n", aspname) >>logf
		return 1
	}
	d_=6
	c_=0
	w_=p
	i_=(q - p) / (n - 1)
	a_=0
	f_=meta[0]
	while ((rc = getline <aspname) > 0) {
		r_=rmeol($0)
		### printf("# Passing\n") >>dbgf
		c_++
		if (c_ <= d_) continue
		r_+=0
		v_=r_ - b
		# XXX TODO: should the below expression be "100 - v_" or just "v_"?
		# printf("%0.8f\t%0.8f\r\n", w_, 100 - v_) >>txtname
		t_=v_
		if (sameunit == 0) {
			if (t_ < 0) t_ = 0
			if (t_ > 100) t_ = 100
			t_ /= 100
			t_ = -log(t_) / log(10)
		}
		printf("%0.8f\t%0.8f\r\n", w_, t_) >>txtname
		if (avgf != "") {
			if (f_ != 0 && a_ >= meta[0]) {
				printf("Mismatched record count in '%s' invalidates average.\n") >>logf
				avgf=""
			}
			if (avgf != "" && f_ != 0 && w_ != keys[a_]) {
				printf("Mismatched wavenumbers in '%s' invalidates average.\n") >>logf
				avgf=""
			}
			if (avgf != "") {
				if (f_ == 0) keys[a_] = w_
				data[a_++] += t_
			}
		}
		w_+=i_
	}
	if (rc < 0) {
		printf("Fatal: %s\n", ERRNO) >>logf
		exit
	}
	if (f_ == 0) meta[0] = a_
	meta[1]++
	### printf("# Done\n") >>dbgf
	return 0
}

BEGINFILE {
	aspname=FILENAME
	bi="background_noise"
	ni="linecount"
	pi="wavenum_start"
	qi="wavenum_end"
	glob[bi]=0
	glob[ni]=0
	glob[pi]=0
	glob[qi]=0

	## printf("@ Start = %s\n", aspname) >>dbgf

	txtname=aspname
	if (txtname == "-") {
		printf("Ignoring standard input.\n") >>logf
		txtname=""
	}
	if (txtname != "" && asp2txt_check(aspname, glob, bi, ni, pi, qi) != 0) {
		printf("Skipping unexpected data from input file '%s'\n", aspname) >>logf
		txtname=""
	}
	if (txtname != "") {
		### printf("# Ready\n") >>dbgf
		nch=length(txtname)
		if (nch < 4) ext=""
		if (nch >= 4) ext=substr(txtname, 1 + nch - 4)
		if (ext != ".asp" && ext != ".ASP") {
			printf("Skipping non-ASP file '%s'.\n", aspname) >>logf
			txtname=""
		}
		if (txtname != "") txtname=substr(txtname, 1, nch - 4) ".txt"
	}
	if (txtname != "" && getline <txtname >= 0) {
		printf("Refusing to overwrite existing output file '%s'.\n", txtname) >>logf
		txtname=""
	}
	if (txtname == "") {
		ec=1
	} else {
		close(aspname)
		if (asp2txt_convert(aspname, txtname, avgf, meta, keys, data, glob[bi], glob[ni], glob[pi], glob[qi]) == 0) {
			# printf("Successfully converted '%s' to '%s'.\n", aspname, txtname) >>dbgf
		} else {
			printf("Failed to convert '%s' to '%s'.\n", aspname, txtname) >>logf
		}
	}
}

# Main logic - implicit loop per record.
{ }

ENDFILE {
	## printf("@ Finish = %s\n", FILENAME) >>dbgf
}

END {
	if (avgf != "" && meta[0] != 0 && meta[1] != 0) {
		for (i = 0; i < meta[0]; i++) {
			printf("%0.8f\t%0.8f\r\n", keys[i], data[i] / meta[1]) >>avgf
		}
	}
	exit ec
}

#!/usr/bin/env cat
